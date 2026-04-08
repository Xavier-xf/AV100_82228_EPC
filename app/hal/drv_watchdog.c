/**
 * @file    drv_watchdog.c
 * @brief   硬件看门狗驱动实现
 */
#define LOG_TAG "DrvWdt"
#include "log.h"

#include "drv_watchdog.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define WATCHDOG_DEV_PATH    "/dev/watchdog"
#define WATCHDOG_IOCTL_BASE  'W'
#define WDIOC_KEEPALIVE      _IOR(WATCHDOG_IOCTL_BASE, 5, int)
#define WDIOC_SETTIMEOUT     _IOWR(WATCHDOG_IOCTL_BASE, 6, int)
#define WDIOC_GETTIMEOUT     _IOR(WATCHDOG_IOCTL_BASE, 7, int)

static int s_wdt_fd      = -1;
static int s_timeout_sec = 0;

int DrvWdtOpen(unsigned int timeout_sec)
{
    if (s_wdt_fd >= 0) { LOG_W("already opened"); return -1; }
    if (timeout_sec < 1) { LOG_W("invalid timeout"); return -1; }

    s_wdt_fd = open(WATCHDOG_DEV_PATH, O_RDONLY);
    if (s_wdt_fd < 0) { LOG_E("open %s fail", WATCHDOG_DEV_PATH); return -1; }

    s_timeout_sec = (int)timeout_sec;
    if (ioctl(s_wdt_fd, WDIOC_SETTIMEOUT, &s_timeout_sec) != 0) {
        ioctl(s_wdt_fd, WDIOC_GETTIMEOUT, &s_timeout_sec);
        LOG_W("set timeout fail, actual=%ds", s_timeout_sec);
    }
    LOG_I("opened, timeout=%ds", s_timeout_sec);
    return 0;
}

int DrvWdtFeed(void)
{
    if (s_wdt_fd < 0) return -1;
    if (ioctl(s_wdt_fd, WDIOC_KEEPALIVE, &s_timeout_sec) != 0) {
        LOG_E("feed fail");
        return -1;
    }
    return 0;
}

int DrvWdtClose(void)
{
    if (s_wdt_fd < 0) return -1;
    s_timeout_sec = -1;
    ioctl(s_wdt_fd, WDIOC_SETTIMEOUT, &s_timeout_sec);
    close(s_wdt_fd);
    s_wdt_fd = -1;
    LOG_I("closed");
    return 0;
}
