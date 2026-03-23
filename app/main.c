/**
 * @file    main.c
 * @brief   门口机系统入口
 *
 * 初始化顺序（严格按依赖关系从底层到上层）：
 *   1. common  — 事件总线（无任何依赖，最先初始化）
 *   2. hal     — 硬件驱动（不依赖 service / app）
 *   3. service — 平台服务（依赖 hal）
 *   4. app     — 业务逻辑（依赖 service，在此完成事件订阅）
 *   5. 驱动使能 — 需要回调的驱动最后 Init，确保回调函数已就绪
 *   6. 主循环  — 仅喂狗，所有业务均由事件驱动
 *
 * 若某个模块 Init 失败，记录日志后继续启动，保证其他功能可用。
 */
#include <stdio.h>
#include <unistd.h>

/* ---- common ---- */
#include "event_bus.h"

/* ---- hal ---- */
#include "drv_platform.h"   /* AK SDK + 软件心跳看门狗（最先 include）*/
#include "drv_watchdog.h"
#include "drv_gpio.h"

/* ---- service ---- */
#include "svc_network.h"
/* ---- app ---- */
#include "app_intercom.h"
#include "app_upgrade.h"

/* =========================================================
 *  常量
 * ========================================================= */
#define WDT_TIMEOUT_SEC      10   /* 硬件看门狗超时（秒）*/
#define MAIN_LOOP_INTERVAL   5    /* 主循环间隔（秒），需 < WDT_TIMEOUT/2 */

/* 设备 ID（对应原版 NetworkDevice 枚举）*/
#define DEVICE_OUTDOOR_1     7
#define DEVICE_OUTDOOR_2     8

/* 统一的初始化宏：失败打印警告，但不 abort */
#define INIT_MODULE(fn)                                            \
    do                                                             \
    {                                                              \
        int _ret = (fn);                                           \
        if (_ret != 0)                                             \
            printf("[MAIN] WARNING: %s returned %d\n", #fn, _ret); \
        else                                                       \
            printf("[MAIN] %s ok\n", #fn);                         \
    } while (0)
/* AK SDK 初始化失败则直接退出（无 AK SDK 后续所有模块都会崩溃）*/
#define INIT_CRITICAL(fn)                                    \
    do                                                       \
    {                                                        \
        if ((fn) != 0)                                       \
        {                                                    \
            printf("[MAIN] CRITICAL FAIL: %s, exit\n", #fn); \
            return -1;                                       \
        }                                                    \
        printf("[MAIN] %s ok\n", #fn);                       \
    } while (0)


int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("\n\n###############################################\n");
    printf("# Door Entry System  Build: %s %s\n", __DATE__, __TIME__);
    printf("###############################################\n\n");

    /* ================================================== */
    /* Step 0: AK SDK 初始化                               */
    /*                                                     */
    /* 必须最先执行，是所有 ak_* API 的运行前提。           */
    /* 对应原版：AKPlatformSdkInit()                        */
    /* ================================================== */
    INIT_CRITICAL(DrvPlatformInit());

    /* ================================================== */
    /* Step 1: 软件系统心跳看门狗                           */
    /*                                                     */
    /* 在 AK SDK 之后立即启动，监控后续 Init 是否会卡死。   */
    /* 若超过 10s 未喂（DrvSystemTickFeed），调用 exit(0)   */
    /* 触发硬件看门狗重启。                                  */
    /* ================================================== */
    DrvSystemTickInit();

    /* ================================================== */
    /* Step 2: 拨码开关读取                                 */
    /*                                                     */
    /* 在 SvcNetworkInit 之前设置设备 ID。                  */
    /* SvcNetworkInit 内部读 s_local_dev 设置 ifconfig IP。 */
    /* ================================================== */
    int door_id = DrvGpioDipSwRead();   /* 1=DOOR1  2=DOOR2 */
    uint8_t dev_id = (uint8_t)(door_id == 2 ? DEVICE_OUTDOOR_2 : DEVICE_OUTDOOR_1);

    printf("[MAIN] Device = DOOR%d (dev_id=%d, ip=192.168.37.%d)\n",
           door_id, dev_id, dev_id);


    SvcNetworkLocalDeviceSet(dev_id);    /* 在网络服务启动前设置设备 ID（决定 ifconfig 使用哪个 IP）*/

    /* ================================================== */
    /* Step 3: common 层                                   */
    /* ================================================== */
    EventBusInit();
    printf("[MAIN] EventBus ready\n");

    /* ================================================== */
    /* Step 4: HAL 层                                      */
    /*                                                     */
    /* 顺序：看门狗 → GPIO → 音频 → 红外 → 视频            */
    /* 注意：DrvAdcInit / DrvKeypadInit / DrvRc522Init 等  */
    /*       产生回调的驱动，在 App 注册回调之后再启动。    */
    /* ================================================== */


    INIT_MODULE(DrvWdtOpen(WDT_TIMEOUT_SEC));    /* 硬件看门狗（我们用 10s）*/


    INIT_MODULE(DrvGpioInit());    /* GPIO 引脚初始化（LED/继电器/功放，对应原版 LightGpioInit + LockGpioInit）*/

    /* -------------------------------------------------- */
    /* Service 层                                       */
    /* -------------------------------------------------- */
    INIT_MODULE(SvcNetworkInit());
    /* -------------------------------------------------- */
    /*  App 层（先注册回调和事件订阅）                    */
    /* -------------------------------------------------- */
    INIT_MODULE(AppIntercomInit());

    INIT_MODULE(AppUpgradeInit());    /* 固件升级（清理残留临时文件）*/

    /* -------------------------------------------------- */
    /* 使能产生回调的驱动（App 层已就绪后再启动）         */
    /* -------------------------------------------------- */

    /* -------------------------------------------------- */
    /* 启动完成提示音                                    */
    /* -------------------------------------------------- */

    printf("\n###############################################\n");
    printf("# System Ready. Device=DOOR%d\n", door_id);
    printf("###############################################\n\n");


    /* ================================================== */
    /* Step 9: 主循环                                      */
    /*                                                     */
    /* 同时喂两个看门狗：                                   */
    /*   硬件看门狗：DrvWdtFeed()          — 防止硬件超时重启 */
    /*   软件心跳狗：DrvSystemTickFeed()   — 防止主循环僵死   */
    /*                                                     */
    /* ================================================== */
    while (1) {
        DrvWdtFeed();
        DrvSystemTickFeed();   /* 喂软件心跳看门狗*/
        sleep(MAIN_LOOP_INTERVAL);
    }

    return 0;
}
