/**
 * @file    drv_platform.h
 * @brief   AK 芯片平台 SDK 初始化 + 软件系统心跳看门狗
 *
 * ===================== 调用约束 =====================
 *
 *  DrvPlatformInit() 必须是全程序第一个被调用的函数，
 *  在 main() 开头，在任何 ak_* API 调用之前执行。
 *
 *  AK SDK 初始化 (ak_sdk_init) 是所有 AK 硬件模块的前提：
 *    ak_ai_* / ak_ao_* / ak_vi_* / ak_venc_* / ak_svp_*
 *    ak_mrd_* / ak_drv_wdt / ak_get_ostime 等
 *
 *  若不调用 DrvPlatformInit()，上述所有 Init 函数都可能崩溃。
 *
 * ===================== 系统心跳看门狗 =====================
 *
 *    DrvSystemTickInit()  — 启动监控线程
 *    DrvSystemTickFeed()  — 喂软件看门狗（在主循环中每秒调用）
 */
#ifndef _DRV_PLATFORM_H_
#define _DRV_PLATFORM_H_

/**
 * @brief AK 平台 SDK 初始化
 *
 * 执行：
 *   ak_sdk_init(&config)                   初始化 AK SDK 运行环境
 *   ak_print_set_level(MODULE_ID_SVP, 7)   设置 SVP 日志级别
 *
 * 编译选项（通过 -D 传入）：
 *   ITS_ENABLE=1  启用 ISP 调试工具服务器（量产时关闭）
 *   ATS_ENABLE=1  启用 Audio 调试工具服务器（量产时关闭）
 *
 * @return 0=成功  -1=失败（系统无法正常工作，建议直接退出）
 */
int DrvPlatformInit(void);

/**
 * @brief 启动软件系统心跳监控线程
 *
 * 启动后台线程，每 100ms 检查心跳时间戳。
 * 若超过 10s 未喂（DrvSystemTickFeed 未被调用），调用 exit(0) 触发重启。
 *
 * 必须在 DrvPlatformInit() 之后调用（依赖 AK 时间接口）。
 */
void DrvSystemTickInit(void);

/**
 * @brief 喂软件看门狗
 *
 * 在主循环中每次喂硬件狗时同时调用此函数。
 * 最长间隔不超过 5s（主循环 sleep(5)，硬件狗 10s，软件狗 10s）。
 */
void DrvSystemTickFeed(void);

#endif /* _DRV_PLATFORM_H_ */
