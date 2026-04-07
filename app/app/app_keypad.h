/**
 * @file    app_keypad.h
 * @brief   数字键盘业务逻辑（菜单状态机）
 *
 * 移植自旧版 NumericKeypad.h / NumericKeypad.c。
 * 由 drv_keypad.c 驱动层回调触发，完成密码开锁、管理菜单等功能。
 */
#ifndef _APP_KEYPAD_H_
#define _APP_KEYPAD_H_

#define KEYPAD_BUFFER_SIZE  8   /* 按键缓冲区最大长度（含 # 号）*/

/**
 * @brief 初始化键盘业务（注册 HAL 回调、复位状态机）
 * @return 0=成功
 */
int AppKeypadInit(void);

/**
 * @brief 由 app_card.c 在修改卡密码模式（TMR_MODIFY_CODE_CARD）刷卡时调用，
 *        存储当前待修改的卡片索引，以便键盘后续验证旧密码。
 */
void AppKeypadSetModifyCardIdx(int idx);

#endif /* _APP_KEYPAD_H_ */
