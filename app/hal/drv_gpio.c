/**
 * @file    drv_gpio.c
 * @brief   门口机 GPIO 业务驱动（灯光 / 电锁 / 功放 / IRCUT / 拨码）
 */
#define LOG_TAG "DrvGpio"
#include "log.h"

#include "drv_gpio.h"
#include "drv_gpio_sysfs.h"
#include <unistd.h>

/* =========================================================
 *  ★ GPIO 引脚号配置（根据实际 PCB 确认）★
 * ========================================================= */
/* 灯光 */
#define PIN_KEY1_LIGHT     24   /* 呼叫按键1 指示灯   */
#define PIN_KEY2_LIGHT     50   /* 呼叫按键2 指示灯   */
#define PIN_KEYPAD_BL      32   /* 数字键盘背光       */
#define PIN_CARD_LIGHT     27   /* 刷卡指示灯         */
#define PIN_INFRARED_LIGHT 58   /* 红外补光灯         */

/* 继电器 */
#define PIN_LOCK_DOOR      37   /* 单元门锁继电器     */
#define PIN_LOCK_GATE      34   /* 围墙门/闸继电器    */

/* 功放 */
#define PIN_AMP_EN         69   /* 功放使能           */

/* IRCUT 滤光片电机 */
#define PIN_IRCUT_INA      65   /* IRCUT 电机 A 相（夜视：LOW）  */
#define PIN_IRCUT_INB      66   /* IRCUT 电机 B 相（夜视：HIGH） */

/* 拨码开关 */
#define PIN_DIP_SW         26   /* DOOR1/DOOR2 选择拨码         */

/* =========================================================
 *  GPIO：拨码开关读取（在 DrvGpioInit 之前也可独立调用）
 * ========================================================= */
/**
 * @brief 读取编码拨码开关，判断本机是 DOOR1 还是 DOOR2
 * @return 1=DOOR1(IP .7)  2=DOOR2(IP .8)
 */
int DrvGpioDipSwRead(void)
{
    GpioSysfsOpen(PIN_DIP_SW, GPIO_DIR_IN, true);

    GpioLevel level = GPIO_LEVEL_UNKNOWN;
    if (!GpioSysfsLevelGet(PIN_DIP_SW, &level)) {
        LOG_W("DipSw read fail, default DOOR1");
        return 1;
    }

    int door_id = (level == GPIO_LEVEL_LOW) ? 1 : 2;
    LOG_I("DipSw GPIO%d=%s => DOOR%d",
          PIN_DIP_SW, (level == GPIO_LEVEL_LOW) ? "LOW" : "HIGH", door_id);
    return door_id;
}

/* =========================================================
 *  DrvGpioInit
 * ========================================================= */
int DrvGpioInit(void)
{
    /* 灯光引脚：输出，默认低电平 */
    GpioSysfsOpen(PIN_KEY1_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_KEY2_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_KEYPAD_BL,      GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_CARD_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_INFRARED_LIGHT, GPIO_DIR_LOW, false);

    /* IRCUT 电机：输出，默认低电平 */
    GpioSysfsOpen(PIN_IRCUT_INA, GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_IRCUT_INB, GPIO_DIR_LOW, false);

    /* 继电器：输出，默认低电平 */
    GpioSysfsOpen(PIN_LOCK_DOOR, GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_LOCK_GATE, GPIO_DIR_LOW, false);

    /* 功放：输出，默认低电平 */
    GpioSysfsOpen(PIN_AMP_EN, GPIO_DIR_LOW, false);

    DrvGpioAmpDisable();
    LOG_I("init ok");
    return 0;
}

/* =========================================================
 *  灯光控制
 * ========================================================= */
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

/* =========================================================
 *  继电器（开锁/开闸）
 *  开/关分离为两个接口，定时器由 app_access.c 管理
 * ========================================================= */
void DrvGpioLockSet(GpioLockType type, int on)
{
    int pin = (type == GPIO_LOCK_GATE) ? PIN_LOCK_GATE : PIN_LOCK_DOOR;
    GpioSysfsLevelSet(pin, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
    LOG_I("%s %s",
          (type == GPIO_LOCK_GATE) ? "GATE" : "LOCK",
          on ? "OPEN" : "CLOSE");
}

int DrvGpioLockOpen(GpioLockType type, int duration_ms)
{
    DrvGpioLockSet(type, 1);
    usleep((unsigned int)duration_ms * 1000u);
    DrvGpioLockSet(type, 0);
    return 0;
}

/* =========================================================
 *  功放控制
 * ========================================================= */
void DrvGpioAmpEnable(void)
{
    GpioSysfsLevelSet(PIN_AMP_EN, GPIO_LEVEL_HIGH);
}

void DrvGpioAmpDisable(void)
{
    GpioSysfsLevelSet(PIN_AMP_EN, GPIO_LEVEL_LOW);
}

/* =========================================================
 *  IRCUT 滤光片电机控制
 *
 *  用法：DrvGpioIrcutNight/Day() 给电机一个方向脉冲，
 *        100ms 后由定时器回调调用 DrvGpioIrcutStop() 停止电机。
 * ========================================================= */

/** 夜视：INB=HIGH INA=LOW（切换到夜视滤光片）*/
void DrvGpioIrcutNight(void)
{
    GpioSysfsLevelSet(PIN_IRCUT_INA, GPIO_LEVEL_LOW);
    GpioSysfsLevelSet(PIN_IRCUT_INB, GPIO_LEVEL_HIGH);
}

/** 白天：INA=HIGH INB=LOW（切换到白天滤光片）*/
void DrvGpioIrcutDay(void)
{
    GpioSysfsLevelSet(PIN_IRCUT_INA, GPIO_LEVEL_HIGH);
    GpioSysfsLevelSet(PIN_IRCUT_INB, GPIO_LEVEL_LOW);
}

/** 电机停止（脉冲结束后调用，两相均置低）*/
void DrvGpioIrcutStop(void)
{
    GpioSysfsLevelSet(PIN_IRCUT_INA, GPIO_LEVEL_LOW);
    GpioSysfsLevelSet(PIN_IRCUT_INB, GPIO_LEVEL_LOW);
}
