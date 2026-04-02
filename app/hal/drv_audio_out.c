/**
 * @file    drv_audio_out.c
 * @brief   音频输出驱动
 */
#include "drv_audio_out.h"
#include "ak_ao.h"
#include "ak_common_audio.h"
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* =========================================================
 *  常量
 * ========================================================= */
#define PCM_VOLUME_BUF_MAX  4096  

/* =========================================================
 *  模块状态结构体
 * ========================================================= */
typedef struct {
    pthread_mutex_t           lock;
    int                       handle_id;
    int                       volume;   /* 当前音量 [1-99]，默认 90 */
    struct ak_audio_out_param param;
} DrvAudioOutCtx;

static DrvAudioOutCtx s_ao = {
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .handle_id = -1,
    .volume    = 90,
};

/* =========================================================
 *  PCM 音量缩放
 *
 *  公式：dB 增益 = volume - 96，线性倍数 = 10^(dB/20)
 *  例：volume=90 → gain ≈ 0.501（轻微衰减）
 *      volume=96 → gain = 1.000（原始电平）
 *
 *  src 数据原地修改（in-place）。
 * ========================================================= */
static void pcm_volume_scale(unsigned char *src, int size, int volume)
{
    unsigned char tmp[PCM_VOLUME_BUF_MAX];  /* 栈上分配，避免两线程（对讲+语音）踩同一静态缓冲 */
    if (size > PCM_VOLUME_BUF_MAX || size <= 0) return;

    float multiplier = powf(10.0f, (float)(volume - 96) / 20.0f);
    memset(tmp, 0, (size_t)size);

    for (int i = 0; i < size; i += 2) {
        short sample = (short)((src[i + 1] << 8) | (src[i] & 0xFF));
        int   scaled = (int)((float)sample * multiplier);

        if      (scaled >  32767) scaled =  32767;
        else if (scaled < -32768) scaled = -32768;

        tmp[i]     = (unsigned char)( scaled        & 0xFF);
        tmp[i + 1] = (unsigned char)((scaled >> 8)  & 0xFF);
    }
    memcpy(src, tmp, (size_t)size);
}

/* =========================================================
 *  音频参数设置
 * ========================================================= */
static void setup_params(int handle_id)
{
    /* 降噪（default_ao_nr_attr = {-40, 0, 1}）*/
    struct ak_audio_nr_attr nr = {-40, 0, 1};
    ak_ao_set_nr_attr(handle_id, &nr);

    /* 自动电平控制（default_ao_aslc_attr = {16384, 8, 0}）*/
    struct ak_audio_aslc_attr aslc = {16384, 8, 0};
    ak_ao_set_aslc_attr(handle_id, &aslc);

    /* EQ（default_ao_eq_attr，来自 ak_audio_config.h）*/
    /* AO EQ 参数（来自 ak_audio_config.h default_ao_eq_attr）
     * 字段名对应 ak_common_audio.h struct ak_audio_eq_attr */
    struct ak_audio_eq_attr eq = {
        .pre_gain   = 1024,
        .bands      = 5,
        .bandfreqs  = {1000, 4500, 4200, 1000, 1250, 0, 0, 0, 0, 0},
        .bandgains  = {-2048, -8192, -16384, 0, 0, 0, 0, 0, 0, 0},
        .bandQ      = {716, 614, 614, 716, 716, 0, 0, 0, 0, 0},
        .band_types = {TYPE_HPF, TYPE_PF1, TYPE_HSF, TYPE_PF1, TYPE_PF1,
                       TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1},
        .enable     = 1,
        .band_enable = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    };
    ak_ao_set_eq_attr(handle_id, &eq);

    /* 增益（default_ao_gain = 2）*/
    ak_ao_set_gain(handle_id, 2);

    /* ak_ao_set_volume：旧版 SetupAudioOutputArgument 未调用，
     * 音量由软件 pcm_volume_scale 控制，不设硬件等级以避免双重叠加 */

    printf("[DrvAudioOut] params set ok\n");
}

/* =========================================================
 *  公开接口
 * ========================================================= */

/**
 * @brief 初始化音频输出设备
 *只做 open + setup，输出循环在 svc_audio.c 中
 */
int DrvAudioOutInit(void)
{
    pthread_mutex_lock(&s_ao.lock);
    if (s_ao.handle_id >= 0) {
        pthread_mutex_unlock(&s_ao.lock);
        return 0;   /* 已初始化 */
    }

    s_ao.param.dev_id                    = DEV_DAC;
    s_ao.param.pcm_data_attr.sample_rate = AK_AUDIO_SAMPLE_RATE_16000;
    s_ao.param.pcm_data_attr.channel_num = AUDIO_CHANNEL_MONO;
    s_ao.param.pcm_data_attr.sample_bits = AK_AUDIO_SMPLE_BIT_16;

    if (ak_ao_open(&s_ao.param, &s_ao.handle_id) != 0) {
        printf("[DrvAudioOut] ak_ao_open fail\n");
        s_ao.handle_id = -1;
        pthread_mutex_unlock(&s_ao.lock);
        return -1;
    }

    setup_params(s_ao.handle_id);

    pthread_mutex_unlock(&s_ao.lock);
    printf("[DrvAudioOut] init ok handle=%d\n", s_ao.handle_id);
    return 0;
}

/**
 * @brief 写入 PCM 帧
 *
 *   pcm_volume_scale(data, len, volume);   ← 同 Pcm16bitVolumeCover
 *   ak_ao_send_frame(handle, data, len, NULL);  ← 完全一致
 *
 * 注意：data 会被原地修改（音量缩放），调用方（svc_audio）负责传入可写缓冲。
 */
int DrvAudioOutWrite(unsigned char *data, unsigned int len)
{
    pthread_mutex_lock(&s_ao.lock);
    int handle = s_ao.handle_id;
    int vol    = s_ao.volume;
    pthread_mutex_unlock(&s_ao.lock);

    if (handle < 0 || !data || len == 0) return -1;

    /* 原地音量缩放*/
    pcm_volume_scale(data, (int)len, vol);

    /* 写入 AK AO 硬件（*/
    return ak_ao_send_frame(s_ao.handle_id, data, (int)len, NULL);
}

/**
 * @brief 设置音量
 */
void DrvAudioOutSetVolume(int volume)
{
    /* volume<=0 或 volume==100 直接返回 */
    if (volume <= 0 || volume == 100) return;

    pthread_mutex_lock(&s_ao.lock);
    s_ao.volume = volume;
    pthread_mutex_unlock(&s_ao.lock);
}

/**
 * @brief 查询 AO 缓冲区剩余数据量
 */
int DrvAudioOutRemainLen(void)
{
    pthread_mutex_lock(&s_ao.lock);
    int handle = s_ao.handle_id;
    pthread_mutex_unlock(&s_ao.lock);

    if (handle < 0) return 0;

    struct ak_dev_buf_status st;
    ak_ao_get_buf_status(handle, &st);
    return st.buf_remain_len;
}

/**
 * @brief 轻量级重启 AO
 *
 * 先 cancel（立即停止播放并清空硬件缓冲），再 restart（恢复运行）。
 * 不关闭设备，避免反复 open/close 带来的延迟。
 * 在播放结束后调用，防止 AO 缓冲区长时间积累。
 */
void DrvAudioOutRestart(void)
{
    pthread_mutex_lock(&s_ao.lock);
    if (s_ao.handle_id >= 0) {
        ak_ao_cancel(s_ao.handle_id);
        ak_ao_restart(s_ao.handle_id);
    }
    pthread_mutex_unlock(&s_ao.lock);
}

/**
 * @brief 检测 AO 缓冲区是否异常溢出
 *
 * buf_remain_len > buf_total_len << 1 表示缓冲区处于异常状态
 * （正常情况下 remain_len 不会超过 total_len）。
 * 检测到溢出时，上层应调用 DrvAudioOutDeinit + DrvAudioOutInit 完整重启。
 */
bool DrvAudioOutIsOverflow(void)
{
    pthread_mutex_lock(&s_ao.lock);
    int handle = s_ao.handle_id;
    pthread_mutex_unlock(&s_ao.lock);

    if (handle < 0) return false;

    struct ak_dev_buf_status st;
    if (ak_ao_get_buf_status(handle, &st) != 0) return false;
    return st.buf_remain_len > (st.buf_total_len << 1);
}

int DrvAudioOutDeinit(void)
{
    pthread_mutex_lock(&s_ao.lock);
    if (s_ao.handle_id >= 0) {
        ak_ao_close(s_ao.handle_id);
        s_ao.handle_id = -1;
    }
    pthread_mutex_unlock(&s_ao.lock);
    return 0;
}
