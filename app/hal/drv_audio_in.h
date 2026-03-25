/**
 * @file    drv_audio_in.h
 * @brief   音频采集驱动（麦克风输入）
 *
 * 职责：
 *   - 封装 AK AI SDK 初始化与帧采集
 *   - 通过回调将 PCM 帧送给上层（不关心帧去哪里）
 *   - 动态开关采集（对讲时开，空闲时关，省电）
 */
#ifndef _DRV_AUDIO_IN_H_
#define _DRV_AUDIO_IN_H_

typedef struct {
    unsigned char      *data;
    unsigned int        len;
    unsigned long long  ts_ms;
    unsigned long       seq;
} AudioInFrame;

/** PCM 帧回调，由驱动线程调用，处理要快，禁止阻塞 */
typedef void (*AudioInFrameCb)(const AudioInFrame *frame);

/** 注册帧回调（Init 之前调用） */
void DrvAudioInSetCallback(AudioInFrameCb cb);

/** 初始化驱动并启动采集线程（线程常驻，采集可动态开关） */
int DrvAudioInInit(void);

/** 开始采集（对讲建立时调用） */
int DrvAudioInStart(void);

/** 停止采集（对讲结束时调用） */
int DrvAudioInStop(void);

/** 反初始化 */
int DrvAudioInDeinit(void);

#endif /* _DRV_AUDIO_IN_H_ */
