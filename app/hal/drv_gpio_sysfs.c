/**
 * @file    drv_gpio_sysfs.c
 * @brief   通用 GPIO sysfs 操作实现
 */
#define LOG_TAG "GpioSysfs"
#include "log.h"

#include "drv_gpio_sysfs.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define PATH_MAX_LEN 64

bool GpioSysfsOpen(int pin, GpioDir dir, bool pull_enable)
{
    char path[PATH_MAX_LEN], value[PATH_MAX_LEN];
    int fd;

    /* 若 gpio%d 目录不存在则导出 */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    if (access(path, F_OK) != 0) {
        snprintf(path, sizeof(path), "/sys/class/gpio/export");
        fd = open(path, O_WRONLY);
        if (fd < 0) { LOG_E("open export fail pin=%d", pin); return false; }
        snprintf(value, sizeof(value), "%d\n", pin);
        if (write(fd, value, strlen(value)) < 0) {
            close(fd);
            LOG_E("export write fail pin=%d", pin);
            return false;
        }
        close(fd);
    }

    /* 设置方向 */
    const char *dir_str[] = {"in", "out", "high", "low"};
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) { LOG_E("direction open fail pin=%d", pin); return false; }
    int len = snprintf(value, sizeof(value), "%s", dir_str[dir]);
    if (write(fd, value, len) < 0) { close(fd); return false; }
    close(fd);

    /* 上下拉（节点不存在时忽略）*/
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/pull_enable", pin);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, pull_enable ? "1" : "0", 1);
        close(fd);
    }
    return true;
}

bool GpioSysfsClose(int pin)
{
    char path[PATH_MAX_LEN], value[PATH_MAX_LEN];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    if (access(path, F_OK) == 0) {
        snprintf(path, sizeof(path), "/sys/class/gpio/unexport");
        int fd = open(path, O_WRONLY);
        if (fd < 0) return false;
        snprintf(value, sizeof(value), "%d\n", pin);
        if (write(fd, value, strlen(value)) < 0) { close(fd); return false; }
        close(fd);
    }
    return true;
}

bool GpioSysfsLevelSet(int pin, GpioLevel level)
{
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) { LOG_E("level set open fail pin=%d", pin); return false; }
    bool ok = (write(fd, (level == GPIO_LEVEL_LOW) ? "0" : "1", 1) > 0);
    close(fd);
    return ok;
}

bool GpioSysfsLevelGet(int pin, GpioLevel *level)
{
    char path[PATH_MAX_LEN], val[4] = {0};
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { LOG_E("level get open fail pin=%d", pin); return false; }
    bool ok = (read(fd, val, 1) > 0);
    close(fd);
    if (ok) *level = (val[0] == '0') ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
    return ok;
}

int GpioSysfsEdge(int pin, GpioEdge edge)
{
    const char *edge_str[] = {"none", "rising", "falling", "both"};
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/edge", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) { LOG_E("edge open fail pin=%d", pin); return -1; }
    int ret = (write(fd, edge_str[edge], strlen(edge_str[edge])) > 0) ? 0 : -1;
    close(fd);
    return ret;
}
