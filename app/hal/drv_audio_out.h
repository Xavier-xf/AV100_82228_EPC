/**
 * @file    drv_audio_out.h
 * @brief   音频输出驱动
 *
 * 改动说明：
 *   - DrvAudioOutWrite 第一参数改为 unsigned char*（可写），
 *     内部做原地音量缩放后再写 AK AO，无需外部分配副本。
 */
#ifndef _DRV_AUDIO_OUT_H_
#define _DRV_AUDIO_OUT_H_

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

/** @brief 反初始化 */
int  DrvAudioOutDeinit(void);

#endif /* _DRV_AUDIO_OUT_H_ */
