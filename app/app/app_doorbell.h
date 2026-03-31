/**
 * @file    app_doorbell.h
 * @brief   门铃按键业务（原 AdcDetectEvent.c 重写）
 *
 * 职责：
 *   - 向 DrvAdc 注册电压回调
 *   - 将 ADC 电压值映射为按键事件（Call / Exit）
 *   - 通过 EventBus 发布事件，不直接调用网络/语音/灯光模块
 */
#ifndef _APP_DOORBELL_H_
#define _APP_DOORBELL_H_

/** @brief 初始化门铃按键检测 */
int AppDoorbellInit(void);

#endif /* _APP_DOORBELL_H_ */
