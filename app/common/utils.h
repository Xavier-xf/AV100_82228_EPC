/**
 * @file    utils.h
 * @brief   通用工具函数
 *
 * 与硬件无关的辅助工具，所有层均可使用。
 */
#ifndef _UTILS_H_
#define _UTILS_H_

#include <time.h>
#include <stdint.h>

/** @brief 获取 last_time 到当前的毫秒差，不修改 last_time */
unsigned long long UtilsDiffMs(struct timespec *last_time);

/** @brief 将 ts 更新为当前单调时间 */
void UtilsGetTime(struct timespec *ts);

/** @brief 返回当前单调时间的毫秒戳 */
uint64_t UtilsMonoMs(void);

/** @brief 返回编译时间（首次调用时解析 __DATE__ / __TIME__）*/
struct tm UtilsCompileTime(void);

/** @brief 居中打印字符串，左右以 [ ] 对齐到 width 列（键盘菜单调试用）*/
void UtilsPrintCentered(const char *str, int width);

#endif /* _UTILS_H_ */
