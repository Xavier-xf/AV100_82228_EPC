/**
 * @file    app_doorbell.c
 * @brief   门铃按键业务实现
 *
 * 通过 EventBus 解耦按键检测与上层响应：
 *   - 呼叫按键 → 发布 EVT_CALL_KEY_PRESSED（网络/语音/灯光各自订阅）
 *   - 退出按键 → 发布 EVT_EXIT_BTN_PRESSED（app_access.c 订阅后处理开锁）
 */
#define LOG_TAG "Doorbell"
#include "log.h"

#include "app_doorbell.h"
#include "event_bus.h"
#include "drv_adc.h"
#include "svc_timer.h"
#include "svc_voice.h"
#include "svc_network.h"
#include "svc_net_manage.h"
#include "drv_gpio.h"
#include <stdlib.h>

/* =========================================================
 *  ADC 按键映射表（电压值单位 mV，容差 ±100mV）
 * ========================================================= */
#define KEY_VOLTAGE_TOLERANCE  100
#define KEY_IDLE_VOLTAGE       3050   /* 无按键时的默认高电平 */

typedef enum {
    KEY_CALL_1 = 0,
    KEY_CALL_2,
    KEY_EXIT_1,
    KEY_EXIT_2,
    KEY_MAP_MAX
} KeyMapIndex;

typedef struct {
    const char *name;
    int         voltage_mv;
    int         is_exit;       /* 1=退出按钮（直接开锁），0=呼叫按键 */
} KeyMap;

static const KeyMap s_key_map[KEY_MAP_MAX] = {
    [KEY_CALL_1] = { "Call1", 1180, 0 },
    [KEY_CALL_2] = { "Call2",  730, 0 },
    [KEY_EXIT_1] = { "Exit1", 2610, 1 },
    [KEY_EXIT_2] = { "Exit2", 2840, 1 },
};

/* =========================================================
 *  忙音回调（呼叫 3.5s 后未被接听则播放忙音）
 * ========================================================= */
static void on_call_busy_timeout(void *arg)
{
    (void)arg;
    LOG_I("call busy timeout");
    SvcVoicePlaySimple(VOICE_CallBusy, VOICE_VOL_DEFAULT);
}

/* =========================================================
 *  ADC 电压 → 按键判断
 * ========================================================= */
static void on_call_key_pressed(int key_idx)
{
    /* 室内机管理会话期间（添加卡/删除卡等界面）屏蔽呼叫按键，与旧版一致 */
    if (SvcNetManageConnected())
        return;

    LOG_I("call key %d pressed", key_idx + 1);

    /* 1. 发布按键事件（网络模块/灯光模块订阅） */
    EventBusPublish(EVT_CALL_KEY_PRESSED, &key_idx, sizeof(key_idx));

    /* 2. 本地提示音 */
    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);

    /* 3. 忙音定时器（若已在跑则不重复启动） */
    if (!SvcTimerActive(TMR_CALL_BUSY)) {
        SvcTimerSet(TMR_CALL_BUSY, 3500, on_call_busy_timeout, NULL);
    }
}

static void on_exit_btn_pressed(int key_idx)
{
    LOG_I("exit button %d pressed", key_idx + 1);
    EventBusPublish(EVT_EXIT_BTN_PRESSED, &key_idx, sizeof(key_idx));
}

/* =========================================================
 *  DrvAdc 回调：电压去抖 + 分发
 * ========================================================= */
static void adc_voltage_cb(int voltage_mv)
{
    static int s_prev_voltage = KEY_IDLE_VOLTAGE;
    static int s_last_key_idx = -1;

    for (int i = 0; i < KEY_MAP_MAX; i++) {
        int v = s_key_map[i].voltage_mv;
        int prev_hit = (abs(s_prev_voltage - v) < KEY_VOLTAGE_TOLERANCE);
        int curr_hit = (abs(voltage_mv    - v) < KEY_VOLTAGE_TOLERANCE);

        if (prev_hit && curr_hit) {
            /* 连续两次采样均命中同一按键 → 确认按下 */
            if (s_last_key_idx != i) {
                s_last_key_idx = i;
                if (s_key_map[i].is_exit) {
                    on_exit_btn_pressed(i);
                } else {
                    on_call_key_pressed(i);
                }
            }
            s_prev_voltage = voltage_mv;
            return;
        }
    }

    /* 未命中任何按键，重置状态 */
    s_prev_voltage = voltage_mv;
    s_last_key_idx = -1;
}

/* =========================================================
 *  EventBus 订阅：网络通知对讲/监控时点亮对应按键灯
 * ========================================================= */
static void on_call_key_for_light(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    int key_idx = *(const int *)arg;
    if (key_idx == 0)      DrvGpioKey1LightSet(1);
    else if (key_idx == 1) DrvGpioKey2LightSet(1);
}

// static void on_call_end_for_light(EventId id, const void *arg, size_t len)
// {
//     (void)id; (void)arg; (void)len;
//     /* 通话/监控结束，熄灭所有呼叫灯 */
//     DrvGpioKey1LightSet(0);
//     DrvGpioKey2LightSet(0);
// }

/* =========================================================
 *  初始化
 * ========================================================= */
int AppDoorbellInit(void)
{
    /* 向 ADC 驱动注册回调 */
    DrvAdcSetCallback(adc_voltage_cb);

    /* 订阅通话/监控结束事件，熄灭按键灯 */
    EventBusSubscribe(EVT_CALL_KEY_PRESSED, on_call_key_for_light);
    // EventBusSubscribe(EVT_NET_CALL_END,     on_call_end_for_light);

    LOG_I("init ok");
    return 0;
}
