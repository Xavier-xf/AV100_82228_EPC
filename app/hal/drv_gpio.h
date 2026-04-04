/**
 * @file    drv_gpio.h
 * @brief   GPIO 硬件抽象层 —— 灯光 / 电锁 / 功放使能
 *
 * 规则：
 *   - 此层只操作寄存器/sys节点，不含任何业务逻辑
 *   - 不依赖任何 service 层或 app 层头文件
 */
#ifndef _DRV_GPIO_H_
#define _DRV_GPIO_H_

/* =========================================================
 *  电锁 / 电控门类型
 * ========================================================= */
typedef enum {
    GPIO_LOCK_DOOR  = 0,   /* 单元门锁 */
    GPIO_LOCK_GATE  = 1,   /* 围墙门 / 闸机 */
} GpioLockType;

/* =========================================================
 *  初始化
 * ========================================================= */
int DrvGpioInit(void);

/* =========================================================
 *  呼叫按键指示灯
 * ========================================================= */
/** @brief 刷卡指示灯 GPIO 27 */
void DrvGpioCardLightSet(int on);
void DrvGpioKey1LightSet(int on);
void DrvGpioKey2LightSet(int on);
void DrvGpioKeypadLightSet(int on);   /* 数字键盘背光 */

/* =========================================================
 *  编码拨码开关（DOOR1/DOOR2 选择）
 * ========================================================= */

/**
 * @brief 读取门口机编号拨码开关
 *
 * 硬件：拨码开关连接到一个 GPIO 引脚（PIN_DIP_SW）。
 *   LOW  (拨码到 DOOR1 侧) → 返回 1 → DEVICE_OUTDOOR_1 (IP: 192.168.37.7)
 *   HIGH (拨码到 DOOR2 侧) → 返回 2 → DEVICE_OUTDOOR_2 (IP: 192.168.37.8)
 *
 * 注意：此函数在 DrvGpioInit() 之前也可独立调用，
 *       内部直接读 sysfs，不依赖 DrvGpioInit 的初始化结果。
 *
 * @return 1=DOOR1  2=DOOR2
 */
int DrvGpioDipSwRead(void);

/* =========================================================
 *  门锁驱动
 *   duration_ms: 开锁持续时间（毫秒）
 * ========================================================= */
int DrvGpioLockOpen(GpioLockType type, int duration_ms);

/* =========================================================
 *  功放使能（Speaker Amplifier）
 * ========================================================= */
/** @brief 红外补光灯控制（GPIO 58）*/
void DrvGpioInfraredLightSet(int on);

void DrvGpioAmpEnable(void);
void DrvGpioAmpDisable(void);

/* =========================================================
 *  IRCUT 滤光片电机（GPIO 65/66）
 *  对应旧版 InfraredDetect.c IRCUT_INA/INB_GPIO
 *  用法：先调 Night/Day 给方向脉冲，100ms 后调 Stop
 * ========================================================= */
void DrvGpioIrcutNight(void);  /* 夜视：INB=HIGH INA=LOW  */
void DrvGpioIrcutDay(void);    /* 白天：INA=HIGH INB=LOW  */
void DrvGpioIrcutStop(void);   /* 停止：两相均低           */

#endif /* _DRV_GPIO_H_ */
