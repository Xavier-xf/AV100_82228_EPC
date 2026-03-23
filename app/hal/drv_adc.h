/**
 * @file    drv_adc.h
 * @brief   ADC 采集驱动层
 *
 * 职责：
 *   - 周期性采集 ADC 电压值
 *   - 将电压值通过回调上报，不做按键映射（映射在 app 层）
 */
#ifndef _DRV_ADC_H_
#define _DRV_ADC_H_

/**
 * @brief ADC 电压上报回调
 * @param voltage_mv  当前采集到的电压值（mV）
 */
typedef void (*DrvAdcCallback)(int voltage_mv);

/**
 * @brief 注册 ADC 数据回调（初始化前调用）
 */
void DrvAdcSetCallback(DrvAdcCallback cb);

/**
 * @brief 初始化 ADC 采集，启动采集线程
 */
int DrvAdcInit(void);

/**
 * @brief 反初始化，停止采集线程
 */
int DrvAdcDeinit(void);

#endif /* _DRV_ADC_H_ */
