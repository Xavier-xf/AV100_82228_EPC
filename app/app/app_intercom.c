/**
 * @file    app_intercom.c
 * @brief   对讲业务状态机
 */
#include "app_intercom.h"
#include "svc_svp.h"
#include "app_upgrade.h"
#include "event_bus.h"
#include "utils.h"
#include "svc_network.h"
#include "svc_voice.h"
#include "svc_timer.h"
#include "svc_intercom_stream.h"
#include "drv_gpio.h"
#include "drv_infrared.h"
#include "drv_video_in.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* ---- 对讲状态（互斥锁保护）---- */
typedef struct {
    pthread_mutex_t lock;
    IntercomState   state;
} AppIntercomCtx;

static AppIntercomCtx s_icom = {
    .lock  = PTHREAD_MUTEX_INITIALIZER,
    .state = INTERCOM_STATE_IDLE,
};

IntercomState AppIntercomGetState(void)
{
    pthread_mutex_lock(&s_icom.lock);
    IntercomState v = s_icom.state;
    pthread_mutex_unlock(&s_icom.lock);
    return v;
}

/* ---- 内部：进入/退出流 ---- */
static void enter_stream(IntercomState new_state, uint8_t peer_dev)
{
    pthread_mutex_lock(&s_icom.lock);

    /* ★ 已经在目标状态，无需重复操作 */
    if (s_icom.state == new_state) {
        pthread_mutex_unlock(&s_icom.lock);
        return;
    }

    /* ★ 允许从 MONITORING 升级到 TALKING*/
    if (s_icom.state == INTERCOM_STATE_MONITORING && new_state == INTERCOM_STATE_TALKING) {
        s_icom.state = INTERCOM_STATE_TALKING;
        pthread_mutex_unlock(&s_icom.lock);

        printf("[AppIntercom] upgrade MONITORING → TALKING peer=0x%02X\n", peer_dev);
        SvcIntercomStreamUpgradeToTalk();
        return;
    }

    /* 只有 IDLE 才能进入新状态 */
    if (s_icom.state != INTERCOM_STATE_IDLE) {
        pthread_mutex_unlock(&s_icom.lock);
        return;
    }

    s_icom.state = new_state;
    pthread_mutex_unlock(&s_icom.lock);

    StreamMode mode = (new_state == INTERCOM_STATE_TALKING)
                    ? STREAM_MODE_TALK : STREAM_MODE_MONITOR;
    printf("[AppIntercom] enter state=%d mode=%d peer=0x%02X\n",
           new_state, mode, peer_dev);

    DrvGpioKey1LightSet(1);
    DrvGpioKey2LightSet(1);
    /* 进入监控/通话时，夜间模式下开红外补光灯（对应旧版 InfraredLightControl(DarkModeStatus())）*/
    if (DrvInfraredIsNight())
        DrvGpioInfraredLightSet(1);
    SvcTimerStop(TMR_CALL_BUSY);
    SvcIntercomStreamStart(mode, peer_dev);
}

static void exit_stream(void)
{
    pthread_mutex_lock(&s_icom.lock);
    if (s_icom.state == INTERCOM_STATE_IDLE) { pthread_mutex_unlock(&s_icom.lock); return; }
    s_icom.state = INTERCOM_STATE_IDLE;
    pthread_mutex_unlock(&s_icom.lock);
    printf("[AppIntercom] exit → IDLE\n");
    SvcIntercomStreamStop();
    DrvGpioKey1LightSet(0);
    DrvGpioKey2LightSet(0);
    /* 退出时关红外补光灯（对应旧版 CommunicateOuttime → InfraredLightControl(0)）*/
    DrvGpioInfraredLightSet(0);
}

/* ---- 事件回调 ---- */
static void on_call_start(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;

    const NetCallArg *call = (const NetCallArg *)arg;
    uint8_t my_dev = SvcNetworkLocalDeviceGet();
    if (call->channel != 0 && (my_dev != (uint8_t)(0x06 + call->channel))) {
        printf("[AppIntercom] call not for me\n"); return;
    }
    SvcTimerStop(TMR_CALL_BUSY);
    enter_stream(INTERCOM_STATE_TALKING, call->sender_dev);
}

static void on_call_end(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    printf("[AppIntercom] HANG received, wait watchdog timeout\n");
}

static void on_stream_status(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;

    const NetStreamStatus *st = (const NetStreamStatus *)arg;
    IntercomState cur = AppIntercomGetState();

    /* 关键帧请求：旧版在流控最前面处理，确保首包即可拿到 IDR */
    if (st->key_frame_req) SvcIntercomStreamRequestKeyFrame();

    if (st->leave_msg_enable) {
        /* 留言提示音：跟室内机设置的语言走；旧版防抖 3000ms */
        static struct timespec last_leave_time = {0};
        unsigned long long diff_ms = UtilsDiffMs(&last_leave_time);
        if (diff_ms > 3000) {
            int lang_idx = st->leave_msg_lang;
            VoiceId vid = (VoiceId)(VOICE_LeaveMsgEng + lang_idx);
            SvcVoicePlaySimple(vid, VOICE_VOL_DEFAULT);
        }
        // 获取当前时间并更新
        UtilsGetTime(&last_leave_time);
    }
    if (cur == INTERCOM_STATE_IDLE) {
        enter_stream(INTERCOM_STATE_MONITORING, st->sender_dev);
        cur = AppIntercomGetState();
    }
    if (cur == INTERCOM_STATE_TALKING || cur == INTERCOM_STATE_MONITORING) {
        SvcIntercomStreamRefresh(st);
        return;
    }
}

static void on_stream_watchdog(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    printf("[AppIntercom] stream watchdog → force stop\n");
    exit_stream();
}

static void on_heartbeat_send(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    IntercomState cur = AppIntercomGetState();
    /* svp_active  = SVP 近期有检测，对应 TimerEnablestatus(SVPTimer)
     * comm_active 对应旧版 TimerEnablestatus(CommunicateTimer)：
     *   旧版仅在通话（OutdoorTalkEvent）和 TUYA_MONITOR_ENABLE 时才置 1，
     *   留言模式（MonitorTimer）时为 0。新版与旧版保持一致：仅 TALKING 时为 1。
     *   若改为 MONITORING 也置 1，室内机会误判为已在通话中，
     *   停止发送 StreamStatus → 看门狗超时 → 监控立即退出。 */
    SvcNetworkStreamStatusSend(
        (uint8_t)(SvcSvpIsActive() ? 1 : 0),
        (uint8_t)((cur == INTERCOM_STATE_TALKING) ? 1 : 0));
}

static void on_call_key_for_network(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    int key_idx = *(const int *)arg;
    int security = SvcTimerActive(TMR_SECURITY_TRIGGER) ? 0 : 1;
    int streaming = SvcIntercomStreamActive() ? 1 : 0;
    SvcNetworkDoorbellNotify(key_idx, security | (streaming << 1));
}

static void on_upgrade_cmd(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    const NetUpgradeArg *upg = (const NetUpgradeArg *)arg;

    if (upg->is_long_pack) {
        /* 长包：数据帧
         * arg1 = 包序号（DP[0..3]）, arg2 = 数据长度（DP[4..7]）
         * data = DP[8..] */
        AppUpgradeHandleLongPack(upg->sender_dev,
                                  upg->arg1, upg->arg2, upg->data);
    } else {
        /* 短包：控制帧
         * ctrl_arg0 = Arg[0], ctrl_arg1 = Arg[1] */
        AppUpgradeHandleCtrlPack(upg->sender_dev,
                                   upg->ctrl_arg0, upg->ctrl_arg1);
    }
}

static void on_motion_sensitivity(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    int sens = *(const int *)arg;
    printf("[AppIntercom] set motion sensitivity=%d\n", sens);
    SvcSvpSetSensitivity(sens);
}

/* ---- 红外夜视事件回调 ---- */

/* IRCUT 电机 100ms 脉冲停止（对应旧版 IrCurClose 定时器回调）*/
static void ircut_close_cb(void *arg)
{
    (void)arg;
    DrvGpioIrcutStop();
}

/**
 * 夜视模式（对应旧版 InfraredDebounce 中 StableLevel=1 的分支）：
 *   1. 切换视频为夜视（灰度），对应 VideoSwitchMode(1)
 *   2. 驱动 IRCUT 电机到夜视位，100ms 后停止
 *   3. 若当前在监控/通话中，开红外补光灯
 */
static void on_infrared_night(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    printf("[AppIntercom] IR → 夜视\n");
    DrvVideoInSwitchMode(1);
    DrvGpioIrcutNight();
    SvcTimerSet(TMR_IRCUT_CLOSE, 100, ircut_close_cb, NULL);
    if (AppIntercomGetState() != INTERCOM_STATE_IDLE)
        DrvGpioInfraredLightSet(1);
}

/**
 * 白天模式（对应旧版 InfraredDebounce 中 StableLevel=0 的分支）：
 *   1. 切换视频为白天（彩色），对应 VideoSwitchMode(0)
 *   2. 驱动 IRCUT 电机到白天位，100ms 后停止
 *   3. 关红外补光灯（白天不需要）
 */
static void on_infrared_day(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    printf("[AppIntercom] IR → 白天\n");
    DrvVideoInSwitchMode(0);
    DrvGpioIrcutDay();
    SvcTimerSet(TMR_IRCUT_CLOSE, 100, ircut_close_cb, NULL);
    DrvGpioInfraredLightSet(0);
}

int AppIntercomInit(void)
{
    EventBusSubscribe(EVT_CALL_KEY_PRESSED,         on_call_key_for_network);
    EventBusSubscribe(EVT_NET_HEARTBEAT_SEND,        on_heartbeat_send);
    EventBusSubscribe(EVT_NET_CALL_START,            on_call_start);
    EventBusSubscribe(EVT_NET_CALL_END,              on_call_end);
    EventBusSubscribe(EVT_NET_STREAM_STATUS,         on_stream_status);
    EventBusSubscribe(EVT_NET_UPGRADE_CMD,           on_upgrade_cmd);
    EventBusSubscribe(EVT_NET_MOTION_SENSITIVITY,    on_motion_sensitivity);
    EventBusSubscribe(EVT_INTERCOM_STREAM_WATCHDOG,  on_stream_watchdog);
    EventBusSubscribe(EVT_INFRARED_NIGHT_MODE,       on_infrared_night);
    EventBusSubscribe(EVT_INFRARED_DAY_MODE,         on_infrared_day);
    printf("[AppIntercom] init ok\n");
    return 0;
}
