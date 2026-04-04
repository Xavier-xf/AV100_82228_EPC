/**
 * @file    utils.h
 * @brief   通用工具函数（原 GeneralInterface.c）
 *
 * 提供时钟工具、居中打印等与硬件无关的辅助函数。
 * 所有模块均可 include 此头文件。
 */
#ifndef _UTILS_H_
#define _UTILS_H_

#include <time.h>

/** @brief 获取两次调用之间的毫秒差（原 DiffClockTimeMs）*/
unsigned long long UtilsDiffMs(struct timespec *last_time);

/** @brief 更新时间戳（原 GetClockTimeMs）*/
void UtilsGetTime(struct timespec *ts);

/** @brief 获取编译时间（原 FetchCompileTime）*/
struct tm UtilsCompileTime(void);

/** @brief 居中打印（用于键盘菜单调试，原 PrintCentered）*/
void UtilsPrintCentered(const char *str, int width);

#endif /* _UTILS_H_ */
