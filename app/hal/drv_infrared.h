/**
 * @file    drv_infrared.h
 * @brief   红外夜视 HAL：光敏传感器监控 + EventBus 事件发布
 *
 * 职责：
 *   - 初始化光敏输入 GPIO（PIN_IR_FEED=70）
 *   - 500ms 轮询光敏电平，1s 去抖后稳定
 *   - 状态变化时发布 EVT_INFRARED_NIGHT_MODE / EVT_INFRARED_DAY_MODE
 *   - 不做任何 IRCUT 电机或视频模式业务（由订阅方处理）
 *
 * 调用时机：
 *   必须在 EventBusInit() 之后、AppIntercomInit()（订阅者注册）之后调用，
 *   以确保初始状态事件能被正确处理。
 */
#ifndef _DRV_INFRARED_H_
#define _DRV_INFRARED_H_

/**
 * @brief 初始化红外夜视检测
 *   - 初始化光敏 GPIO
 *   - 立即读取当前状态并发布初始事件
 *   - 启动 500ms 轮询线程（状态变化后 1s 去抖再发布事件）
 * @return 0=成功 -1=GPIO 初始化失败
 */
int DrvInfraredInit(void);

/**
 * @brief 读取当前是否处于夜间模式（直接读 GPIO，无去抖）
 * @return 1=夜间  0=白天
 */
int DrvInfraredIsNight(void);

#endif /* _DRV_INFRARED_H_ */
