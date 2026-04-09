/**
 * @file    app_intercom.c
 * @brief   对讲业务状态机
 *
 * 状态：IDLE → CALLING → TALKING / MONITORING
 *   IDLE       ：无对讲活动
 *   MONITORING ：室内机发起监控（单向视频，门口机不发音频）
 *   TALKING    ：室内机接听，双向音视频
 *
 * 允许升级路径：MONITORING → TALKING（保持流不断，升级编码参数）
 * 禁止降级路径：TALKING → MONITORING
 */
#define LOG_TAG "Intercom"
#include "log.h"

#include "app_intercom.h"
#include "app_user_config.h"
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
#ifdef KEYPAD_ENABLE
#include "app_keypad.h"
#endif
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* =========================================================
 *  对讲状态（互斥锁保护）
 * ========================================================= */
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

/* =========================================================
 *  内部：进入/退出流
 * ========================================================= */
static void enter_stream(IntercomState new_state, uint8_t peer_dev)
{
    pthread_mutex_lock(&s_icom.lock);

    /* 已在目标状态，忽略重复请求 */
    if (s_icom.state == new_state) {
        pthread_mutex_unlock(&s_icom.lock);
        return;
    }

    /* MONITORING → TALKING：升级流模式，保持连接不中断 */
    if (s_icom.state == INTERCOM_STATE_MONITORING && new_state == INTERCOM_STATE_TALKING) {
        s_icom.state = INTERCOM_STATE_TALKING;
        pthread_mutex_unlock(&s_icom.lock);
        LOG_I("upgrade MONITORING → TALKING peer=0x%02X", peer_dev);
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
    LOG_I("enter state=%d mode=%d peer=0x%02X", new_state, mode, peer_dev);

    DrvGpioKey1LightSet(1);
    DrvGpioKey2LightSet(1);
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

    LOG_I("exit → IDLE");
    SvcIntercomStreamStop();
    DrvGpioKey1LightSet(0);
    DrvGpioKey2LightSet(0);
    DrvGpioInfraredLightSet(0);
}

/* =========================================================
 *  事件回调
 * ========================================================= */

/* 室内机接听（OutdoorTalkEvent 0x57）
 * channel 字段：1=DOOR1  2=DOOR2，0=广播（不过滤）*/
static void on_call_start(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    const NetCallArg *call = (const NetCallArg *)arg;
    uint8_t my_dev = SvcNetworkLocalDeviceGet();
    if (call->channel != 0 && (my_dev != (uint8_t)(0x06 + call->channel))) {
        LOG_D("call not for me (ch=%d my=0x%02X)", call->channel, my_dev);
        return;
    }
    SvcTimerStop(TMR_CALL_BUSY);
    enter_stream(INTERCOM_STATE_TALKING, call->sender_dev);
}

/* 室内机挂断（OutdoorHangEvent 0x75）：等流看门狗自然超时，保持与室内机逻辑一致 */
static void on_call_end(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    LOG_D("HANG received, wait stream watchdog");
}

/* 流控/监控保活（StreamStatusEvent 0x59）*/
static void on_stream_status(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    const NetStreamStatus *st = (const NetStreamStatus *)arg;
    IntercomState cur = AppIntercomGetState();

    if (st->key_frame_req) SvcIntercomStreamRequestKeyFrame();

    if (st->leave_msg_enable) {
        /* 留言提示音防抖：3s 内不重复播放 */
        static struct timespec last_leave_time = {0};
        unsigned long long diff_ms = UtilsDiffMs(&last_leave_time);
        if (diff_ms > 3000) {
            VoiceId vid = (VoiceId)(VOICE_LeaveMsgEng + st->leave_msg_lang);
            SvcVoicePlaySimple(vid, VOICE_VOL_DEFAULT);
        }
        UtilsGetTime(&last_leave_time);
    }

    if (cur == INTERCOM_STATE_IDLE) {
        enter_stream(INTERCOM_STATE_MONITORING, st->sender_dev);
        cur = AppIntercomGetState();
    }
    if (cur == INTERCOM_STATE_TALKING || cur == INTERCOM_STATE_MONITORING) {
        SvcIntercomStreamRefresh(st);
    }
}

/* 流看门狗超时：强制结束对讲 */
static void on_stream_watchdog(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    LOG_W("stream watchdog → force stop");
    exit_stream();
}

/* 心跳回包（IdRepeatEvent 0x55 → case 0）
 *   svp_active  = SVP 近期有检测活动
 *   comm_active = 仅通话中（TALKING）时为 1；监控中为 0，
 *                 否则室内机会误判为通话中并停发 StreamStatus → 看门狗超时 */
static void on_heartbeat_send(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    IntercomState cur = AppIntercomGetState();
    SvcNetworkStreamStatusSend(
        (uint8_t)(SvcSvpIsActive() ? 1 : 0),
        (uint8_t)((cur == INTERCOM_STATE_TALKING) ? 1 : 0));
}

/* 门铃按键网络通知（DoorbellEvent 0x56）*/
static void on_call_key_for_network(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    int key_idx = *(const int *)arg;
    int security  = SvcTimerActive(TMR_SECURITY_TRIGGER) ? 0 : 1;
    int streaming = SvcIntercomStreamActive() ? 1 : 0;
    SvcNetworkDoorbellNotify(key_idx, security | (streaming << 1));
}

/* 固件升级命令分发 */
static void on_upgrade_cmd(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    const NetUpgradeArg *upg = (const NetUpgradeArg *)arg;
    if (upg->is_long_pack) {
        AppUpgradeHandleLongPack(upg->sender_dev, upg->arg1, upg->arg2, upg->data);
    } else {
        AppUpgradeHandleCtrlPack(upg->sender_dev, upg->ctrl_arg0, upg->ctrl_arg1);
    }
}

/* 移动侦测灵敏度设置（MotionSensitivityEvent 0x63）*/
static void on_motion_sensitivity(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    int sens = *(const int *)arg;
    LOG_I("set motion sensitivity=%d", sens);
    SvcSvpSetSensitivity(sens);
}

/* =========================================================
 *  红外夜视事件回调
 * ========================================================= */

/* IRCUT 电机脉冲停止（100ms 后由定时器触发）*/
static void ircut_close_cb(void *arg)
{
    (void)arg;
    DrvGpioIrcutStop();
}

/**
 * 切换至夜视模式：
 *   1. 视频切灰度
 *   2. IRCUT 电机拨至夜视位，100ms 后停止
 *   3. 通话/监控中开红外补光灯
 *   4. 键盘背光按夜间时长点亮
 */
static void on_infrared_night(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    LOG_D("IR → 夜视");
    DrvVideoInSwitchMode(1);
    DrvGpioIrcutNight();
    SvcTimerSet(TMR_IRCUT_CLOSE, 100, ircut_close_cb, NULL);
    if (AppIntercomGetState() != INTERCOM_STATE_IDLE)
        DrvGpioInfraredLightSet(1);
#ifdef KEYPAD_ENABLE
    AppKeypadLightEnable();
#endif
}

/**
 * 切换至白天模式：
 *   1. 视频切彩色
 *   2. IRCUT 电机拨至白天位，100ms 后停止
 *   3. 关红外补光灯
 *   4. 键盘背光缩短至 10s
 */
static void on_infrared_day(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    LOG_D("IR → 白天");
    DrvVideoInSwitchMode(0);
    DrvGpioIrcutDay();
    SvcTimerSet(TMR_IRCUT_CLOSE, 100, ircut_close_cb, NULL);
    DrvGpioInfraredLightSet(0);
#ifdef KEYPAD_ENABLE
    AppKeypadLightEnable();
#endif
}

/* 安防触发：自动呼叫室内机（带 security=0 标志，室内机据此区分普通门铃与安防呼叫）*/
static void on_security_triggered(EventId id, const void *arg, size_t len)
{
    (void)id; (void)arg; (void)len;
    LOG_W("security triggered → auto-call indoor unit");
    SvcNetworkDoorbellNotify(0, 1);
}

/* 出厂复位（远程触发，保留语言设置后重启）*/
static void on_net_reset(EventId id, const void *data, size_t len)
{
    (void)id; (void)data; (void)len;
    LOG_W("factory reset triggered by indoor unit");
    AppUserConfigReset();
    sync();
    system("reboot -f");
}

/* =========================================================
 *  初始化
 * ========================================================= */
int AppIntercomInit(void)
{
    EventBusSubscribe(EVT_SYSTEM_SECURITY_TRIGGERED,  on_security_triggered);
    EventBusSubscribe(EVT_NET_RESET_CMD,             on_net_reset);
    EventBusSubscribe(EVT_CALL_KEY_PRESSED,          on_call_key_for_network);
    EventBusSubscribe(EVT_NET_HEARTBEAT_SEND,        on_heartbeat_send);
    EventBusSubscribe(EVT_NET_CALL_START,            on_call_start);
    EventBusSubscribe(EVT_NET_CALL_END,              on_call_end);
    EventBusSubscribe(EVT_NET_STREAM_STATUS,         on_stream_status);
    EventBusSubscribe(EVT_NET_UPGRADE_CMD,           on_upgrade_cmd);
    EventBusSubscribe(EVT_NET_MOTION_SENSITIVITY,    on_motion_sensitivity);
    EventBusSubscribe(EVT_INTERCOM_STREAM_WATCHDOG,  on_stream_watchdog);
    EventBusSubscribe(EVT_INFRARED_NIGHT_MODE,       on_infrared_night);
    EventBusSubscribe(EVT_INFRARED_DAY_MODE,         on_infrared_day);
    LOG_I("init ok");
    return 0;
}
