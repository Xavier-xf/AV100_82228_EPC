/**
 * @file    drv_audio_out.h
 * @brief   音频输出驱动
 *
 * 改动说明：
 *   - DrvAudioOutWrite 第一参数改为 unsigned char*（可写），
 *     内部做原地音量缩放后再写 AK AO，无需外部分配副本。
 *   - 新增 DrvAudioOutRestart：轻量级重启（ak_ao_cancel + ak_ao_restart），
 *     播放结束后调用，清空 AO 内部积压缓冲，避免长时间开启导致缓存越来越长。
 *   - 新增 DrvAudioOutIsOverflow：检测缓冲区异常溢出，溢出时由上层调用
 *     DrvAudioOutDeinit + DrvAudioOutInit 做完整重启。
 */
#ifndef _DRV_AUDIO_OUT_H_
#define _DRV_AUDIO_OUT_H_

#include <stdbool.h>

/** @brief 初始化 AO 设备（开机调用一次）*/
int  DrvAudioOutInit(void);

/**
 * @brief 写入一帧 PCM 数据
 * @param data  PCM 数据（函数内部会原地修改做音量缩放）
 * @param len   数据字节数
 */
int  DrvAudioOutWrite(unsigned char *data, unsigned int len);

/** @brief 设置播放音量 [1-99] */
void DrvAudioOutSetVolume(int volume);

/** @brief 获取 AO 缓冲区剩余字节数 */
int  DrvAudioOutRemainLen(void);

/**
 * @brief 轻量级重启 AO（ak_ao_cancel + ak_ao_restart，不关闭设备）
 *
 * 在播放结束后调用，清空 AO 内部积压缓冲，防止长时间开启导致缓存越来越长。
 * 比 Deinit + Init 代价小，不影响下一次播放的启动速度。
 */
void DrvAudioOutRestart(void);

/**
 * @brief 检测 AO 缓冲区是否异常溢出
 * @return true：缓冲区异常，需完整重启（DrvAudioOutDeinit + DrvAudioOutInit）
 */
bool DrvAudioOutIsOverflow(void);

/** @brief 反初始化（完整关闭 AO 设备）*/
int  DrvAudioOutDeinit(void);

#endif /* _DRV_AUDIO_OUT_H_ */
