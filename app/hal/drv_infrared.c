/**
 * @file    drv_infrared.c
 * @brief   红外夜视 HAL：光敏传感器监控
 *
 * 对应旧版：InfraredDetect.c → InfraredDetectInit_main() + InfraredDetectThread()
 *
 * 差异：
 *   旧版通过 SetTimer(IrFeedTimer) 做去抖，新版在轮询线程内用计数器做等效的
 *   2×500ms=1s 去抖，避免 HAL 层调用 SvcTimer（保持层次清晰）。
 *   IRCUT 电机控制、视频模式切换、补光灯控制由订阅方（app_intercom.c）完成。
 */
#include "drv_infrared.h"
#include "drv_gpio_sysfs.h"
#include "event_bus.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

/* 光敏传感器 GPIO（对应旧版 IR_FEED_GPIO=70）*/
#define PIN_IR_FEED  70

/* 当前稳定状态：1=夜间 0=白天 -1=未知（初始）*/
static int s_stable = -1;

/* =========================================================
 *  DrvInfraredIsNight
 * ========================================================= */
int DrvInfraredIsNight(void)
{
    GpioLevel lv = GPIO_LEVEL_LOW;
    GpioSysfsLevelGet(PIN_IR_FEED, &lv);
    return (lv == GPIO_LEVEL_HIGH) ? 1 : 0;
}

/* =========================================================
 *  内部：状态变化时发布事件
 *  对应旧版 InfraredDebounce() 里的 VideoSwitchMode + InfraredLightControl + IRCUT
 *  ★ 这里只发布事件，业务由 app_intercom.c 的订阅者完成
 * ========================================================= */
static void publish_state(int night)
{
    s_stable = night;
    printf("[DrvInfrared] stable → %s\n", night ? "夜视" : "白天");
    EventBusPublish(night ? EVT_INFRARED_NIGHT_MODE : EVT_INFRARED_DAY_MODE, NULL, 0);
}

/* =========================================================
 *  轮询线程（对应旧版 InfraredDetectThread + IrFeedTimer去抖）
 *  每 500ms 读一次 GPIO，连续 2 次（1s）与当前稳定状态不同才切换
 * ========================================================= */
static void *infrared_poll_thread(void *arg)
{
    (void)arg;
    int pending = -1;   /* 候选新状态 */
    int cnt     = 0;    /* 候选计数   */

    while (1) {
        usleep(500 * 1000);

        int cur = DrvInfraredIsNight();

        if (cur == s_stable) {
            /* 与稳定状态一致：重置候选 */
            pending = -1;
            cnt     = 0;
        } else if (cur == pending) {
            /* 候选状态连续保持 */
            if (++cnt >= 2) {       /* 2×500ms = 1s 稳定 → 确认切换 */
                publish_state(cur);
                pending = -1;
                cnt     = 0;
            }
        } else {
            /* 新的候选状态（可能是抖动）*/
            pending = cur;
            cnt     = 1;
        }
    }
    return NULL;
}

/* =========================================================
 *  DrvInfraredInit
 *  对应旧版 InfraredDetectInit_main()
 * ========================================================= */
int DrvInfraredInit(void)
{
    /* 初始化光敏 GPIO（输入，使能上拉）对应旧版 GpioOpen(70, GPIO_DIR_IN, true) */
    if (!GpioSysfsOpen(PIN_IR_FEED, GPIO_DIR_IN, true)) {
        printf("[DrvInfrared] GPIO%d open fail\n", PIN_IR_FEED);
        return -1;
    }

    /* 读取当前状态，立即发布初始事件（让 IRCUT + 视频模式与物理状态同步）
     * ★ 调用方必须在 AppIntercomInit()（订阅者注册）之后才能调用本函数 */
    int night = DrvInfraredIsNight();
    printf("[DrvInfrared] init: %s\n", night ? "夜视" : "白天");
    publish_state(night);

    /* 启动轮询线程 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, infrared_poll_thread, NULL) != 0) {
        printf("[DrvInfrared] create poll thread fail\n");
        return -1;
    }
    pthread_detach(tid);

    printf("[DrvInfrared] init ok\n");
    return 0;
}
