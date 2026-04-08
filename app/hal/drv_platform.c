/**
 * @file    drv_platform.c
 * @brief   AK 芯片平台 SDK 初始化 + 软件系统心跳看门狗
 *
 * 软件心跳看门狗原理：
 *   监控线程每 100ms 检查心跳时间戳，若超过 10s 未喂（主循环卡死），
 *   调用 exit(0) 主动退出，让系统更快重启（无需等硬件狗超时）。
 *   主循环每 5s 同时调用 DrvWdtFeed() + DrvSystemTickFeed()。
 */
#define LOG_TAG "Platform"
#include "log.h"

#include "drv_platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

#include <ak_common.h>
#include <ak_log.h>

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
#define SYSTEMTICK_TIMEOUT_MS   (10 * 1000)   /* 超时阈值：10s */
#define SYSTEMTICK_CHECK_MS     100            /* 监控间隔：100ms */

static volatile unsigned long long s_tick_timestamp = 0;

static unsigned long long get_ts_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (unsigned long long)tv.tv_sec * 1000ULL
         + (unsigned long long)tv.tv_usec / 1000ULL;
}

static void *systemtick_thread(void *arg)
{
    (void)arg;
    while (1) {
        usleep(SYSTEMTICK_CHECK_MS * 1000);

        unsigned long long now  = get_ts_ms();
        unsigned long long last = s_tick_timestamp;

        if (now > last && (now - last) > SYSTEMTICK_TIMEOUT_MS) {
            LOG_E("system tick timeout (%llus), reboot", (now - last) / 1000);
            fflush(stdout);
            exit(0);
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
        LOG_E("ak_sdk_init fail");
        return -1;
    }

    ak_print_set_level(MODULE_ID_SVP, LOG_LEVEL_RESERVED);

    LOG_I("AK SDK init ok (ITS=%d ATS=%d)", ITS_ENABLE, ATS_ENABLE);
    return 0;
}

void DrvSystemTickInit(void)
{
    s_tick_timestamp = get_ts_ms();

    pthread_t tid;
    if (pthread_create(&tid, NULL, systemtick_thread, NULL) != 0) {
        LOG_E("systemtick thread create fail");
        return;
    }
    pthread_detach(tid);
    LOG_I("systemtick init ok (timeout=%ds)", SYSTEMTICK_TIMEOUT_MS / 1000);
}

void DrvSystemTickFeed(void)
{
    s_tick_timestamp = get_ts_ms();
}
