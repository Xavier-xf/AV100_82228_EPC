/**
 * @file    svc_voice.h
 * @brief   本地语音播放服务（PCM / MP3，消息队列 + 互斥锁）
 *
 * 对应原文件：
 *   VoiceRingPlay.c   → 语音请求投递 + VoiceInfoImport/VoiceDataExport 重定义
 *   VoiceRingTone.c   → VoiceDecodeThread + MP3/PCM 解码核心
 *   VoiceDecode.h     → VoiceInfo / VoiceFrame 数据结构（已整合）
 */
#ifndef _SVC_VOICE_H_
#define _SVC_VOICE_H_

/* =========================================================
 *  语音列表（与原 VoiceRingPlay.h 完全一致）
 * ========================================================= */
#define VOICE_LIST(V)   \
    V(Unlock16k)        \
    V(LeaveMsgEng)  V(LeaveMsgChi)  V(LeaveMsgGer)  V(LeaveMsgHeb) \
    V(LeaveMsgPol)  V(LeaveMsgPor)  V(LeaveMsgSpa)  V(LeaveMsgFre) \
    V(LeaveMsgJap)  V(LeaveMsgIta)  V(LeaveMsgDut)  V(LeaveMsgSlo) \
    V(UnlockEng)    V(UnlockChi)    V(UnlockGer)    V(UnlockHeb)   \
    V(UnlockPol)    V(UnlockPor)    V(UnlockSpa)    V(UnlockFre)   \
    V(UnlockJap)    V(UnlockIta)    V(UnlockDut)    V(UnlockSlo)   \
    V(CallBusy)     V(Bi1)  V(Bi2)  V(Bi3)  V(Bi4)  V(LongBi)  V(Dio)

#define _VOICE_ENUM(n) VOICE_##n,
typedef enum {
    VOICE_LIST(_VOICE_ENUM)
    VOICE_TOTAL
} VoiceId;

#define VOICE_BASE_PATH   "/app/voice/"
#define VOICE_VOL_DEFAULT  90

typedef void (*VoiceEventCb)(void);

/**
 * @brief 播放语音提示（带起止回调）
 * @param id        语音 ID
 * @param volume    音量 [1-99]
 * @param on_start  播放开始回调（NULL 忽略）
 * @param on_end    播放结束回调（NULL 忽略）
 */
void SvcVoicePlay(VoiceId id, int volume,
                  VoiceEventCb on_start, VoiceEventCb on_end);

/** @brief 简便宏（无回调）*/
#define SvcVoicePlaySimple(id, vol) SvcVoicePlay((id), (vol), NULL, NULL)

/**
 * @brief 查询是否正在解码/播放（用于键盘输入防抖）
 * @return 1=繁忙 0=空闲
 */
int SvcVoiceBusy(void);

/** @brief 初始化语音服务 */
int SvcVoiceInit(void);

/** @brief 反初始化 */
int SvcVoiceDeinit(void);

#endif /* _SVC_VOICE_H_ */
