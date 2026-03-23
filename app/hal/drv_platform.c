/**
 * @file    drv_platform.c
 * @brief   AK 芯片平台 SDK 初始化 + 软件系统心跳看门狗
 *
 * 原版 main.c 对应代码：
 *   AKPlatformSdkInit()     → DrvPlatformInit()
 *   systemtick_init()       → DrvSystemTickInit()
 *   reset_system_timestamp()→ DrvSystemTickFeed()
 *   feed_dog()              → 主循环中 DrvWdtFeed() + DrvSystemTickFeed()
 *
 * ===================== 软件心跳看门狗原理 =====================
 *
 *  问题背景：
 *    原版注释"为了防止启动被 load_isp_conf 损坏，增加心跳检测"
 *    load_isp_conf 等初始化操作有时会导致主流程僵死，
 *    硬件看门狗本身需要主动喂，如果主循环卡死则无法喂狗，
 *    系统会在硬件看门狗超时（10s）后重启。
 *    软件心跳额外提供了一层保障，通过 exit(0) 主动退出，
 *    让系统更快重启（不必等硬件狗超时）。
 *
 *  实现：
 *    s_tick_timestamp：上次喂软件狗的时间戳（毫秒）
 *    监控线程每 100ms 检查：若当前时间 - 上次喂狗时间 > 10s → exit(0)
 *    主循环每 5s：DrvWdtFeed() + DrvSystemTickFeed()（同时喂两个狗）
 *
 *  注意：原版使用 ak_get_ostime + ak_diff_ms_time，
 *        新版使用 gettimeofday（更通用，不依赖 AK 时间接口）。
 *        行为完全等效。
 */
#include "drv_platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

/* AK SDK 头文件 */
#include <ak_common.h>   /* ak_sdk_init, sdk_run_config, SDK_RUN_NORMAL, AK_SUCCESS */
#include <ak_log.h>      /* ak_print_set_level, MODULE_ID_SVP */

/* =========================================================
 *  编译选项（通过 Makefile -D 传入）
 * ========================================================= */
#ifndef ITS_ENABLE
#define ITS_ENABLE 0
#endif
#ifndef ATS_ENABLE
#define ATS_ENABLE 0
#endif

/* =========================================================
 *  软件心跳看门狗
 * ========================================================= */
#define SYSTEMTICK_TIMEOUT_MS   (10 * 1000)   /* 10s 超时 → exit(0) */
#define SYSTEMTICK_CHECK_MS     100            /* 监控线程检查间隔 100ms */

static volatile unsigned long long s_tick_timestamp = 0;  /* 上次喂狗时间（ms）*/

/* 获取当前时间戳（毫秒） */
static unsigned long long get_ts_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (unsigned long long)tv.tv_sec * 1000ULL
         + (unsigned long long)tv.tv_usec / 1000ULL;
}

/* 监控线程*/
static void *systemtick_thread(void *arg)
{
    (void)arg;
    while (1) {
        usleep(SYSTEMTICK_CHECK_MS * 1000);

        unsigned long long now  = get_ts_ms();
        unsigned long long last = s_tick_timestamp;

        if (now > last && (now - last) > SYSTEMTICK_TIMEOUT_MS) {
            printf("\n#####################################\n");
            printf("-------- system bad, reboot...\n");
            printf("#####################################\n");
            fflush(stdout);
            exit(0);   /* exit(0) → 硬件看门狗超时 → 系统重启 */
        }
    }
    return NULL;
}

/* =========================================================
 *  公开接口
 * ========================================================= */

int DrvPlatformInit(void)
{
    sdk_run_config config;
    memset(&config, 0, sizeof(config));

    config.mem_trace_flag         = SDK_RUN_NORMAL;
    config.isp_tool_server_flag   = ITS_ENABLE ? 1 : 0;
    config.audio_tool_server_flag = ATS_ENABLE ? 1 : 0;

    if (ak_sdk_init(&config) != AK_SUCCESS) {
        printf("[DrvPlatform] ak_sdk_init FAIL\n");
        return -1;
    }

    /* SVP 日志级别：LOG_LEVEL_DEBUG=最详细（调试用），量产可改为 LOG_LEVEL_NOTICE */
    ak_print_set_level(MODULE_ID_SVP, LOG_LEVEL_DEBUG);

    printf("[DrvPlatform] AK SDK init ok (ITS=%d ATS=%d)\n",
           ITS_ENABLE, ATS_ENABLE);
    return 0;
}

void DrvSystemTickInit(void)
{
    /* 初始化时间戳*/
    s_tick_timestamp = get_ts_ms();

    pthread_t tid;
    if (pthread_create(&tid, NULL, systemtick_thread, NULL) != 0) {
        printf("[DrvPlatform] systemtick thread create fail\n");
        return;
    }
    pthread_detach(tid);
    printf("[DrvPlatform] systemtick init ok (timeout=%ds)\n",
           SYSTEMTICK_TIMEOUT_MS / 1000);
}

void DrvSystemTickFeed(void)
{
    s_tick_timestamp = get_ts_ms();
}
