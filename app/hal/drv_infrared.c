/**
 * @file    drv_infrared.c
 * @brief   红外夜视 HAL：光敏传感器监控
 *
 * 职责：
 *   - 初始化光敏输入 GPIO（PIN_IR_FEED=70）
 *   - 500ms 轮询光敏电平，连续 2 次（1s）稳定后发布事件
 *   - 状态变化时发布 EVT_INFRARED_NIGHT_MODE / EVT_INFRARED_DAY_MODE
 *   - 不做 IRCUT 电机、视频模式、补光灯控制（由订阅方处理）
 */
#define LOG_TAG "Infrared"
#include "log.h"

#include "drv_infrared.h"
#include "drv_gpio_sysfs.h"
#include "event_bus.h"
#include <pthread.h>
#include <unistd.h>

#define PIN_IR_FEED  70   /* 光敏传感器 GPIO */

static int s_stable = -1;  /* 当前稳定状态：1=夜间 0=白天 -1=未知（初始）*/

/* =========================================================
 *  接口
 * ========================================================= */
int DrvInfraredIsNight(void)
{
    GpioLevel lv = GPIO_LEVEL_LOW;
    GpioSysfsLevelGet(PIN_IR_FEED, &lv);
    return (lv == GPIO_LEVEL_HIGH) ? 1 : 0;
}

/* =========================================================
 *  内部：状态变化时发布事件
 * ========================================================= */
static void publish_state(int night)
{
    s_stable = night;
    LOG_I("stable → %s", night ? "night" : "day");
    EventBusPublish(night ? EVT_INFRARED_NIGHT_MODE : EVT_INFRARED_DAY_MODE, NULL, 0);
}

/* =========================================================
 *  轮询线程
 *  每 500ms 读一次 GPIO，连续 2 次与稳定状态不同才切换（1s 去抖）
 * ========================================================= */
static void *infrared_poll_thread(void *arg)
{
    (void)arg;
    int pending = -1;
    int cnt     = 0;

    while (1) {
        usleep(500 * 1000);

        int cur = DrvInfraredIsNight();

        if (cur == s_stable) {
            pending = -1;
            cnt     = 0;
        } else if (cur == pending) {
            if (++cnt >= 2) {
                publish_state(cur);
                pending = -1;
                cnt     = 0;
            }
        } else {
            pending = cur;
            cnt     = 1;
        }
    }
    return NULL;
}

/* =========================================================
 *  初始化
 * ========================================================= */
int DrvInfraredInit(void)
{
    if (!GpioSysfsOpen(PIN_IR_FEED, GPIO_DIR_IN, true)) {
        LOG_E("GPIO%d open fail", PIN_IR_FEED);
        return -1;
    }

    /* 读取当前状态，立即发布初始事件，确保订阅方初始同步
     * 注意：调用方须在订阅者注册（AppIntercomInit）之后才调用本函数 */
    int night = DrvInfraredIsNight();
    LOG_I("init state: %s", night ? "night" : "day");
    publish_state(night);

    pthread_t tid;
    if (pthread_create(&tid, NULL, infrared_poll_thread, NULL) != 0) {
        LOG_E("create poll thread fail");
        GpioSysfsClose(PIN_IR_FEED);
        return -1;
    }
    pthread_detach(tid);

    LOG_I("init ok");
    return 0;
}
