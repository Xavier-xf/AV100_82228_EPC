/**
 * @file    drv_keypad.h
 * @brief   XW12A 数字键盘 HAL 驱动
 *
 * 负责加载内核模块、打开设备、轮询读取原始字节并解码为按键值，
 * 业务逻辑由 app_keypad.c 通过回调处理。
 */
#ifndef _DRV_KEYPAD_H_
#define _DRV_KEYPAD_H_

/* 解码后的按键值 */
#define KEYPAD_KEY_0      0
#define KEYPAD_KEY_1      1
#define KEYPAD_KEY_2      2
#define KEYPAD_KEY_3      3
#define KEYPAD_KEY_4      4
#define KEYPAD_KEY_5      5
#define KEYPAD_KEY_6      6
#define KEYPAD_KEY_7      7
#define KEYPAD_KEY_8      8
#define KEYPAD_KEY_9      9
#define KEYPAD_KEY_STAR   10   /* * 退格键 */
#define KEYPAD_KEY_POUND  11   /* # 确认键 */

/**
 * @brief 按键回调类型
 * @param key  解码后的按键值（KEYPAD_KEY_0 ~ KEYPAD_KEY_POUND）
 */
typedef void (*DrvKeypadCallback)(int key);

/**
 * @brief 注册按键回调（在 DrvKeypadInit 之前调用）
 */
void DrvKeypadSetCallback(DrvKeypadCallback cb);

/**
 * @brief 初始化 XW12A 键盘驱动（加载模块、打开设备、启动轮询线程）
 * @return 0=成功，-1=失败（模块不存在或设备打开失败）
 */
int DrvKeypadInit(void);

#endif /* _DRV_KEYPAD_H_ */
