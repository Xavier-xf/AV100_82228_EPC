/**
 * @file    utils.c
 * @brief   通用工具函数（原 GeneralInterface.c，函数名加 Utils 前缀）
 */
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

unsigned long long UtilsDiffMs(struct timespec *last_time)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // 先算秒差
    long sec_diff  = now.tv_sec  - last_time->tv_sec;
    long nsec_diff = now.tv_nsec - last_time->tv_nsec;

    // 处理纳秒不够减的情况（借位）
    if (nsec_diff < 0) {
        sec_diff--;               // 秒减1
        nsec_diff += 1000000000L; // 纳秒补1秒
    }

    // 最终计算毫秒，安全无溢出
    return (unsigned long long)sec_diff * 1000ULL
         + (unsigned long long)nsec_diff / 1000000ULL;
}

void UtilsGetTime(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

uint64_t UtilsMonoMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

struct tm UtilsCompileTime(void)
{
    static struct tm t;
    static int done = 0;
    if (!done) {
        strptime(__DATE__, "%b %d %Y", &t);
        strptime(__TIME__, "%H:%M:%S", &t);
        done = 1;
    }
    return t;
}

void UtilsPrintCentered(const char *str, int width)
{
    int len = (int)strlen(str);
    int pad = width - len - 2;
    if (pad > 0) {
        int lp = pad / 2, rp = pad - lp;
        printf("[%*s%s%*s]\n", lp, "", str, rp, "");
    } else {
        printf("[%s]\n", str);
    }
}
