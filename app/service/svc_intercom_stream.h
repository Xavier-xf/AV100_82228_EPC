/**
 * @file    svc_intercom_stream.h
 * @brief   对讲媒体流服务（音频 + 视频发送/接收）
 *
 * ===================== 架构说明 =====================
 *
 *  Linux System V 消息队列（msgget / msgsnd / msgrcv）
 *  取代原来的 CircularList 环形队列。
 *
 *  队列拓扑：
 *
 *   ┌─────────────────────────────────────────────────────┐
 *   │          svc_intercom_stream 内部数据流              │
 *   │                                                     │
 *   │  drv_audio_in                                       │
 *   │    └─ AudioInCb ─→ [MQ_AUDIO_TX] ─→ AudioTxThread  │
 *   │                                         │           │
 *   │                                    G711编码          │
 *   │                                         │           │
 *   │                                  NetAudioPackageSend│
 *   │                                                     │
 *   │  AudioRxThread ─→ G711解码 ─→ [MQ_AUDIO_RX] ─→     │
 *   │                                   AudioSpkThread   │
 *   │                                         │           │
 *   │                                  DrvAudioOutWrite   │
 *   │                                                     │
 *   │  drv_video_in                                       │
 *   │    └─ VideoFrameCb ─→ 写入VideoPool ─→              │
 *   │                       [MQ_VIDEO_TX] ─→ VideoTxThread│
 *   │                                         │           │
 *   │                                  NetVideoPackageSend│
 *   └─────────────────────────────────────────────────────┘
 *
 *  消息队列 KEY：
 *    MQ_KEY_AUDIO_TX = 0xDA01  (mic PCM → 网络发送)
 *    MQ_KEY_AUDIO_RX = 0xDA02  (网络接收 → 扬声器)
 *    MQ_KEY_VIDEO_TX = 0xDV01  (视频帧索引 → 网络发送)
 *
 * ===================== 流看门狗 =====================
 *
 *  室内机每 < 5s 发一次 StreamStatus 保活。
 *  若 5s 内未收到，发布 EVT_INTERCOM_STREAM_WATCHDOG，
 *  app_intercom 收到后关闭流。
 *
 * ===================== 网络协议 =====================
 *
 *  音频：Layer2 Raw Socket，协议号 0x2600 + device_id
 *         PCM 16bit 16000Hz → G711 Alaw 压缩后发送
 *  视频：UDP 广播，端口 = 0x1600 + device_id
 *         H264 码流分包发送（每包最大 32KB）
 */
#ifndef _SVC_INTERCOM_STREAM_H_
#define _SVC_INTERCOM_STREAM_H_

#include <stdint.h>

/* =========================================================
 *  流控制：通话模式 / 监控模式
 * ========================================================= */
typedef enum {
    STREAM_MODE_TALK    = 0,   /* 双向：音频发/收 + 视频发    */
    STREAM_MODE_MONITOR = 1,   /* 单向：视频发（室内机看）     */
} StreamMode;

/**
 * @brief 启动媒体流（建立通话或监控时调用）
 * @param mode        通话模式 / 监控模式
 * @param peer_dev_id 室内机设备 ID（用于设置网络协议号）
 */
int SvcIntercomStreamStart(StreamMode mode, uint8_t peer_dev_id);

/**
 * @brief 停止媒体流（挂断或超时时调用）
 */
int SvcIntercomStreamStop(void);

/**
 * @brief 查询流是否正在运行
 * @return 1=运行中 0=已停止
 */
int SvcIntercomStreamActive(void);

/**
 * @brief 收到 StreamStatus 时调用，刷新流看门狗
 * @param status  室内机发来的流控参数
 */
void SvcIntercomStreamRefresh(const void *status);

/**
 * @brief 设置对讲接收音量（StreamStatus 携带）
 * @param raw_vol  原始音量值（0-15），换算后设置 AO
 */
void SvcIntercomStreamSetVolume(uint8_t raw_vol);

/**
 * @brief 请求编码器输出关键帧（室内机请求时调用）
 */
void SvcIntercomStreamRequestKeyFrame(void);


/**
 * @brief 初始化流服务（在 main 中调用，仅一次）
 */
int SvcIntercomStreamInit(void);

#endif /* _SVC_INTERCOM_STREAM_H_ */
