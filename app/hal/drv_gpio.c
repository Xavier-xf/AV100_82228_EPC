/**
 * @file    drv_gpio.c
 * @brief   门口机 GPIO 业务驱动（灯光 / 电锁 / 功放 / 红外补光）
 *
 * 移植说明：
 *   底层 sysfs 操作由 drv_gpio_sysfs.c 提供（GpioSysfsOpen/LevelSet/LevelGet）
 *   此文件只做"引脚号 → 业务语义"的映射，不直接操作 sysfs 字符串路径
 *
 * ★ 引脚号配置区（根据实际硬件修改此处）★
 */
#include "drv_gpio.h"
#include "drv_gpio_sysfs.h"
#include <unistd.h>
#include <stdio.h>

/* =========================================================
 *  ★ 根据实际 PCB 填写 GPIO 引脚号 ★
 * ========================================================= */
#define PIN_KEY1_LIGHT     24   /* Key1 灯 GPIO 24 (LightControl.h) */    /* 呼叫按键1 指示灯   */
#define PIN_KEY2_LIGHT     50   /* Key2 灯 GPIO 50 */    /* 呼叫按键2 指示灯   */
#define PIN_KEYPAD_BL      32   /* 键盘背光 GPIO 32 */    /* 数字键盘背光       */
#define PIN_LOCK_DOOR      63   /* 门锁继电器（TODO: 确认实际引脚）*/    /* 单元门锁继电器     */
#define PIN_LOCK_GATE      64   /* 门闸继电器（TODO: 确认实际引脚）*/    /* 围墙门/闸继电器    */
#define PIN_AMP_EN         65   /* 功放使能 */    /* 功放使能           */
#define PIN_DIP_SW         26   /* 编码拨码开关输入引脚（TODO: 确认实际引脚）*/
#define PIN_CARD_LIGHT     27   /* 刷卡指示灯 GPIO 27 */
#define PIN_INFRARED_LIGHT 58   /* 红外补光灯 GPIO 58 */   /* 红外补光灯 GPIO 58 */    /* 红外补光灯         */

/**
 * @brief 读取编码拨码开关，判断本机是 DOOR1 还是 DOOR2
 *
 * 原版 main.c 逻辑：
 *   GpioOpen(DIP_SW_PIN, GPIO_DIR_IN, 1);
 *   GpioLevelGet(DIP_SW_PIN, &level);
 *   NetworkDevice DevId = level == GPIO_LEVEL_LOW ? DEVICE_OUTDOOR_1 : DEVICE_OUTDOOR_2;
 *
 * 对应设备 ID：
 *   DEVICE_OUTDOOR_1 = 7 (拨码到 DOOR1，GPIO LOW,  IP 192.168.37.7)
 *   DEVICE_OUTDOOR_2 = 8 (拨码到 DOOR2，GPIO HIGH, IP 192.168.37.8)
 */
int DrvGpioDipSwRead(void)
{
    /* 导出并设置为输入 */
    GpioSysfsOpen(PIN_DIP_SW, GPIO_DIR_IN, true);

    GpioLevel level = GPIO_LEVEL_UNKNOWN;
    if (!GpioSysfsLevelGet(PIN_DIP_SW, &level)) {
        printf("[DrvGpio] DipSw read fail, default DOOR1\n");
        return 1;   /* 默认 DOOR1 */
    }

    int door_id = (level == GPIO_LEVEL_LOW) ? 1 : 2;
    printf("[DrvGpio] DipSw GPIO%d=%s → DOOR%d\n",
           PIN_DIP_SW, (level == GPIO_LEVEL_LOW) ? "LOW" : "HIGH", door_id);
    return door_id;
}

int DrvGpioInit(void)
{
    /* 导出并设置方向：输出，初始低电平 */
    GpioSysfsOpen(PIN_KEY1_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_KEY2_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_KEYPAD_BL,      GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_LOCK_DOOR,      GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_LOCK_GATE,      GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_AMP_EN,         GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_CARD_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_INFRARED_LIGHT, GPIO_DIR_LOW, false);

    DrvGpioAmpDisable();
    printf("[DrvGpio] init ok\n");
    return 0;
}

void DrvGpioCardLightSet(int on)
{
    GpioSysfsLevelSet(PIN_CARD_LIGHT, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

void DrvGpioKey1LightSet(int on)
{
    GpioSysfsLevelSet(PIN_KEY1_LIGHT, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

void DrvGpioKey2LightSet(int on)
{
    GpioSysfsLevelSet(PIN_KEY2_LIGHT, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

void DrvGpioKeypadLightSet(int on)
{
    GpioSysfsLevelSet(PIN_KEYPAD_BL, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

void DrvGpioInfraredLightSet(int on)
{
    GpioSysfsLevelSet(PIN_INFRARED_LIGHT, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

int DrvGpioLockOpen(GpioLockType type, int duration_ms)
{
    int pin = (type == GPIO_LOCK_GATE) ? PIN_LOCK_GATE : PIN_LOCK_DOOR;
    printf("[DrvGpio] lock open type=%d duration=%dms\n", type, duration_ms);
    GpioSysfsLevelSet(pin, GPIO_LEVEL_HIGH);
    usleep((unsigned int)(duration_ms * 1000));
    GpioSysfsLevelSet(pin, GPIO_LEVEL_LOW);
    return 0;
}

void DrvGpioAmpEnable(void)
{
    GpioSysfsLevelSet(PIN_AMP_EN, GPIO_LEVEL_HIGH);
}

void DrvGpioAmpDisable(void)
{
    GpioSysfsLevelSet(PIN_AMP_EN, GPIO_LEVEL_LOW);
}
