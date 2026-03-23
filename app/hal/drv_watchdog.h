/**
 * @file    drv_watchdog.h
 * @brief   硬件看门狗驱动
 */
#ifndef _DRV_WATCHDOG_H_
#define _DRV_WATCHDOG_H_

/** @brief 打开看门狗，设置超时秒数 [1, 357] */
int DrvWdtOpen(unsigned int timeout_sec);

/** @brief 喂狗 */
int DrvWdtFeed(void);

/** @brief 关闭看门狗 */
int DrvWdtClose(void);

#endif /* _DRV_WATCHDOG_H_ */
