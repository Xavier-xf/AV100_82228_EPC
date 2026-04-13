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
#define LOG_TAG "Main"
#include "log.h"

#include <unistd.h>
#include <stdint.h>

/* ---- common ---- */
#include "event_bus.h"

/* ---- hal ---- */
#include "drv_platform.h"   /* AK SDK + 软件心跳看门狗*/
#include "drv_watchdog.h"
#include "drv_gpio.h"
#include "drv_infrared.h"
#include "drv_adc.h"
#include "drv_audio_in.h"
#include "drv_video_in.h"
#ifdef CARD_ENABLE
#include "drv_card.h"
#endif

/* ---- service ---- */
#include "svc_timer.h"
#include "svc_audio.h"
#include "svc_voice.h"
#include "svc_network.h"
#include "svc_svp.h"
#include "svc_intercom_stream.h"
#ifdef CARD_ENABLE
#include "svc_net_manage.h"
#endif

/* ---- app ---- */
#include "app_intercom.h"
#include "app_upgrade.h"
#include "app_doorbell.h"
#ifdef CARD_ENABLE
#include "app_card.h"
#endif
#ifdef KEYPAD_ENABLE
#include "app_keypad.h"
#include "drv_keypad.h"
#endif
#include "app_user_config.h"

/* =========================================================
 *  常量
 * ========================================================= */
#define WDT_TIMEOUT_SEC      10   /* 硬件看门狗超时（秒）*/
#define MAIN_LOOP_INTERVAL    5   /* 主循环间隔（秒），需 < WDT_TIMEOUT/2 */

#define DEVICE_OUTDOOR_1      7
#define DEVICE_OUTDOOR_2      8

/* 统一的初始化宏：失败打印警告，但不 abort */
#define INIT_MODULE(fn)                                         \
    do {                                                        \
        int _ret = (fn);                                        \
        if (_ret != 0)                                          \
            LOG_W("WARNING: %s returned %d", #fn, _ret);       \
        else                                                    \
            LOG_I("%s ok", #fn);                                \
    } while (0)

/* AK SDK 初始化失败则直接退出*/
#define INIT_CRITICAL(fn)                                       \
    do {                                                        \
        if ((fn) != 0) {                                        \
            LOG_E("CRITICAL FAIL: %s, exit", #fn);             \
            return -1;                                          \
        }                                                       \
        LOG_I("%s ok", #fn);                                    \
    } while (0)


int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    LOG_RAW("##############################################################\n");
    LOG_RAW("# Door Entry System  Build: %s %s\n", __DATE__, __TIME__);
    LOG_RAW("##############################################################\n");

    /* ================================================
     * Step 0: AK SDK 初始化
     * ================================================ */
    INIT_CRITICAL(DrvPlatformInit());

    /* ================================================
     * Step 1: 软件系统心跳看门狗
     * ================================================ */
    DrvSystemTickInit();

    /* ================================================
     * Step 2: 拨码开关读取
     * ================================================ */
    int door_id = DrvGpioDipSwRead();   /* 1=DOOR1  2=DOOR2 */
    uint8_t dev_id = (uint8_t)(door_id == 2 ? DEVICE_OUTDOOR_2 : DEVICE_OUTDOOR_1);

    LOG_I("Device = DOOR%d (dev_id=%d, ip=192.168.37.%d)",
          door_id, dev_id, dev_id);

    SvcNetworkLocalDeviceSet(dev_id);   /* 在网络服务启动前设置设备 ID */

    /* ================================================
     * Step 3: common 层
     * ================================================ */
    EventBusInit();
    LOG_I("EventBus ready");

    /* ================================================
     * Step 4: HAL 层
     * ================================================ */
    INIT_MODULE(DrvWdtOpen(WDT_TIMEOUT_SEC));    /* 硬件看门狗*/

    INIT_MODULE(DrvGpioInit());         /* GPIO 引脚初始化*/
    INIT_MODULE(DrvAudioInInit());      /* 音频输入*/
    INIT_MODULE(DrvVideoInInit());      /* 视频输入*/

    /* ================================================
     * Step 5: Service 层
     * ================================================ */
    INIT_MODULE(SvcTimerInit());        /* 定时器调度线程 */
    INIT_MODULE(SvcAudioInit());        /* 音频输出队列*/
    INIT_MODULE(SvcVoiceInit());        /* 语音解码播放*/
    INIT_MODULE(SvcNetworkInit());      /* 网络命令通道 */

    /* SVP 人形检测（第三路 320×180，VI_DATA_TYPE_RGB_LINEINTL）*/
    INIT_MODULE(SvcSvpInit(320, 180, 0, VI_DATA_TYPE_RGB_LINEINTL));

    /* 对讲音视频流*/
    INIT_MODULE(SvcIntercomStreamInit());

    /* ================================================
     * Step 6: App 层
     * ================================================ */
    INIT_MODULE(AppDoorbellInit());     /* 门铃按键（注册 ADC 回调）*/
    INIT_MODULE(AppIntercomInit());     /* 对讲状态机 */
    INIT_MODULE(AppUpgradeInit());      /* 固件升级*/

    /* 用户配置（开锁时长/语言/提示音等）*/
    INIT_MODULE(AppUserConfigInit());

    /* IC 卡：先注册回调（AppCardInit），再初始化硬件（DrvCardInit）*/
#ifdef CARD_ENABLE
    INIT_MODULE(AppCardInit());         /* 卡组数据库加载 + 注册回调 */
    INIT_MODULE(SvcNetManageInit());    /* 网络卡片管理（TCP 4321）  */
#endif

    /* 键盘：先注册回调（AppKeypadInit），再初始化硬件（DrvKeypadInit）*/
#ifdef KEYPAD_ENABLE
    INIT_MODULE(AppKeypadInit());       /* 按键状态机 + 注册回调 */
#endif

    /* ================================================
     * Step 7: 使能产生回调的驱动
     * ================================================ */
    INIT_MODULE(DrvAdcInit());          /* ADC 按键采集 */
#ifdef CARD_ENABLE
    INIT_MODULE(DrvCardInit());         /* RC522 读卡器*/
#endif
#ifdef KEYPAD_ENABLE
    INIT_MODULE(DrvKeypadInit());       /* XW12A 键盘*/
#endif

    /* 红外夜视检测（必须在 AppIntercomInit 注册订阅后调用，
     * 因 DrvInfraredInit 会立即发布初始状态事件）*/
    INIT_MODULE(DrvInfraredInit());

    /* ================================================
     * Step 8: 启动完成提示音
     * ================================================ */
    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);

    LOG_RAW("##############################################################\n");
    LOG_RAW("# System Ready. Device=DOOR%d\n", door_id);
    LOG_RAW("##############################################################\n");

    /* ================================================
     * Step 9: 主循环
     * ================================================ */
    while (1) {
        DrvWdtFeed();
        DrvSystemTickFeed();   /* 喂软件心跳看门狗 */
        sleep(MAIN_LOOP_INTERVAL);
    }

    return 0;
}
