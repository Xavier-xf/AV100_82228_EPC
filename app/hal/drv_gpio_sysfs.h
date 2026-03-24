/**
 * @file    drv_gpio_sysfs.h
 * @brief   通用 GPIO sysfs 操作库
 *
 * 提供 sysfs GPIO 的：导出/方向/电平读写/边沿触发
 * 供 drv_gpio.c（门锁/灯光/功放）调用，不含业务逻辑。
 */
#ifndef _DRV_GPIO_SYSFS_H_
#define _DRV_GPIO_SYSFS_H_

#include <stdbool.h>

typedef enum { GPIO_DIR_IN=0, GPIO_DIR_OUT, GPIO_DIR_HIGH, GPIO_DIR_LOW } GpioDir;
typedef enum { NONE_EDGE=0, RISING_EDGE, FALLING_EDGE, BOTH_EDGE }        GpioEdge;
typedef enum { GPIO_LEVEL_LOW=0, GPIO_LEVEL_HIGH=1, GPIO_LEVEL_UNKNOWN }  GpioLevel;

/** 导出并初始化 GPIO（设置方向 + 上下拉）*/
bool GpioSysfsOpen(int pin, GpioDir dir, bool pull_enable);

/** 注销 GPIO */
bool GpioSysfsClose(int pin);

/** 设置电平 */
bool GpioSysfsLevelSet(int pin, GpioLevel level);

/** 读取电平 */
bool GpioSysfsLevelGet(int pin, GpioLevel *level);

/** 设置边沿触发模式（用于 epoll 中断）*/
int  GpioSysfsEdge(int pin, GpioEdge edge);

#endif /* _DRV_GPIO_SYSFS_H_ */
