/**
 * @file    svc_audio.h
 * @brief   音频输出服务
 *
 * ===================== 架构说明 =====================
 *
 *  统一管理所有 PCM 帧的输出队列，两路来源用消息类型区分优先级：
 *
 *    svc_voice（语音解码）  → SvcAudioFeed(AUDIO_SRC_VOICE, ...)
 *                                    │ msgsnd(mtype=1, 高优先)
 *                                    ▼
 *    svc_intercom_stream    → SvcAudioFeed(AUDIO_SRC_INTERCOM, ...)
 *    （对讲接收）                     │ msgsnd(mtype=2, 低优先)
 *                                    ▼
 *                              [MQ_AUDIO_OUT]
 *                                    │
 *                          AudioOutputThread:
 *                            先尝试 mtype=1 (VOICE)
 *                            再尝试 mtype=2 (INTERCOM)
 *                                    │
 *                          DrvGpioAmpEnable()
 *                          DrvAudioOutWrite(pcm, len)
 *                          SvcTimerSet(TMR_AMP_OFF, 3000ms)
 *
 *  VOICE 帧优先消费。
 *  当语音在播放时，INTERCOM 帧在队列中等待；语音结束后恢复对讲音频。
 */
#ifndef _SVC_AUDIO_H_
#define _SVC_AUDIO_H_

#include <stdint.h>

typedef enum {
    AUDIO_SRC_VOICE    = 0,   /* 本地语音提示（高优先）*/
    AUDIO_SRC_INTERCOM = 1,   /* 对讲接收音频（低优先）*/
    AUDIO_SRC_MAX
} AudioSource;

/**
 * @brief 送入一帧 PCM 数据
 *
 * 线程安全。data 内容会被拷贝入消息队列，调用方可立即释放。
 *
 * @param src   数据来源（决定消息队列优先级）
 * @param data  PCM 原始数据
 * @param len   数据字节数
 * @return  0=成功  -1=队列满丢帧
 */
int SvcAudioFeed(AudioSource src, const uint8_t *data, uint32_t len);

/**
 * @brief 清空指定来源的所有残留帧
 *        （通话结束时清空 INTERCOM 残留，切换语音时使用）
 */
void SvcAudioFlush(AudioSource src);

/**
 * @brief 获取 AO 硬件缓冲区剩余字节数（语音解码流控用）
 */
int SvcAudioRemainLen(void);

/**
 * @brief 初始化音频输出服务
 */
int SvcAudioInit(void);

/**
 * @brief 反初始化（销毁消息队列）
 */
int SvcAudioDeinit(void);

#endif /* _SVC_AUDIO_H_ */
