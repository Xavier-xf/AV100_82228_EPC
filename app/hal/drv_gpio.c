/**
 * @file    drv_gpio.c
 * @brief   门口机 GPIO 业务驱动（灯光 / 电锁 / 功放 / IRCUT / 拨码）
 *
 * ★ 引脚号配置区（来源：原版头文件和源文件汇总）★
 *
 * 来源：
 *   LightControl.h  → Card(27) Key1(24) Key2(50) Keypad(32) Infrared(58)
 *   SpeakAmp.c      → AMP(69)
 *   InfraredDetect.c→ IRCUT_INA(65) IRCUT_INB(66) IR_FEED(70)
 *   Unlock.c        → LOCK(37) GATE(34)
 *   main.c          → DIP_SW(26)
 *
 * ⚠️ 注意：
 *   GATE_EXIT_GPIO=27（Unlock.c）与 PIN_CARD_LIGHT=27（LightControl.h）编号相同。
 *   原版使用同一个引脚，出门按钮（GATE_EXIT）与刷卡指示灯（CARD_LIGHT）复用 GPIO27。
 *   实际是否冲突需确认 PCB。新版保留原分配，若有冲突请修改其中一个。
 */
#include "drv_gpio.h"
#include "drv_gpio_sysfs.h"
#include <unistd.h>
#include <stdio.h>

/* =========================================================
 *  ★ GPIO 引脚号配置（根据实际 PCB 确认）★
 * ========================================================= */
/* 灯光（来源：LightControl.h）*/
#define PIN_KEY1_LIGHT     24   /* 呼叫按键1 指示灯   */
#define PIN_KEY2_LIGHT     50   /* 呼叫按键2 指示灯   */
#define PIN_KEYPAD_BL      32   /* 数字键盘背光       */
#define PIN_CARD_LIGHT     27   /* 刷卡指示灯         */
#define PIN_INFRARED_LIGHT 58   /* 红外补光灯         */

/* 继电器（来源：Unlock.c）*/
#define PIN_LOCK_DOOR      37   /* 单元门锁继电器（LOCK_GPIO）  */
#define PIN_LOCK_GATE      34   /* 围墙门/闸继电器（GATE_GPIO） */

/* 功放（来源：SpeakAmp.c: #define APEAK_AMP_GPIO 69）*/
#define PIN_AMP_EN         69   /* 功放使能（APEAK_AMP_GPIO）   */

/* 拨码开关（来源：main.c: GpioOpen(26, GPIO_DIR_IN, true)）*/
#define PIN_DIP_SW         26   /* DOOR1/DOOR2 选择拨码         */

/* =========================================================
 *  GPIO：拨码开关读取（在 DrvGpioInit 之前也可独立调用）
 * ========================================================= */
/**
 * @brief 读取编码拨码开关，判断本机是 DOOR1 还是 DOOR2
 *
 * 原版逻辑（main.c）：
 *   GpioOpen(26, GPIO_DIR_IN, true);
 *   GpioLevelGet(26, &level);
 *   NetworkDevice DevId = level == GPIO_LEVEL_LOW ? DEVICE_OUTDOOR_1 : DEVICE_OUTDOOR_2;
 *
 * @return 1=DOOR1(IP .7)  2=DOOR2(IP .8)
 */
int DrvGpioDipSwRead(void)
{
    GpioSysfsOpen(PIN_DIP_SW, GPIO_DIR_IN, true);

    GpioLevel level = GPIO_LEVEL_UNKNOWN;
    if (!GpioSysfsLevelGet(PIN_DIP_SW, &level)) {
        printf("[DrvGpio] DipSw read fail, default DOOR1\n");
        return 1;
    }

    int door_id = (level == GPIO_LEVEL_LOW) ? 1 : 2;
    printf("[DrvGpio] DipSw GPIO%d=%s → DOOR%d\n",
           PIN_DIP_SW, (level == GPIO_LEVEL_LOW) ? "LOW" : "HIGH", door_id);
    return door_id;
}

/* =========================================================
 *  DrvGpioInit（对应原版 LightGpioInit + LockGpioInit + SpeakAmpGpioInit）
 * ========================================================= */
int DrvGpioInit(void)
{
    /* 灯光引脚：输出，默认低电平（对应原版 LightGpioInit）*/
    GpioSysfsOpen(PIN_KEY1_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_KEY2_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_KEYPAD_BL,      GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_CARD_LIGHT,     GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_INFRARED_LIGHT, GPIO_DIR_LOW, false);

    /* 继电器：输出，默认低电平（对应原版 LockGpioInit）*/
    GpioSysfsOpen(PIN_LOCK_DOOR, GPIO_DIR_LOW, false);
    GpioSysfsOpen(PIN_LOCK_GATE, GPIO_DIR_LOW, false);

    /* 功放：输出，默认低电平（对应原版 SpeakAmpGpioInit）*/
    GpioSysfsOpen(PIN_AMP_EN, GPIO_DIR_LOW, false);

    DrvGpioAmpDisable();
    printf("[DrvGpio] init ok\n");
    return 0;
}

/* =========================================================
 *  灯光控制（对应原版 LightControl.c 宏展开函数）
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
 *
 *  原版 Unlock() 调用：
 *    GpioLevelSet(LOCK_GPIO, GPIO_LEVEL_HIGH);  ← 开
 *    SetTimer(time * 1000, LockTimer, LockColse, NULL);
 *    LockColse: GpioLevelSet(LOCK_GPIO, GPIO_LEVEL_LOW); ← 关
 *
 *  新版：开/关分离为两个接口，定时器由 app_access.c 管理
 * ========================================================= */
void DrvGpioLockSet(GpioLockType type, int on)
{
    int pin = (type == GPIO_LOCK_GATE) ? PIN_LOCK_GATE : PIN_LOCK_DOOR;
    GpioSysfsLevelSet(pin, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
    printf("[DrvGpio] %s %s\n",
           (type == GPIO_LOCK_GATE) ? "GATE" : "LOCK",
           on ? "OPEN" : "CLOSE");
}

/* =========================================================
 *  功放控制（对应原版 SpeakAmpEnable / SpeakAmpDisable）
 * ========================================================= */
int DrvGpioLockOpen(GpioLockType type, int duration_ms)
{
    DrvGpioLockSet(type, 1);
    usleep((unsigned int)duration_ms * 1000u);
    DrvGpioLockSet(type, 0);
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
