/**
 * @file    drv_audio_out.c
 * @brief   音频输出驱动
 *
 *  新版设计：
 *    删除独立 AudioOutputThread：改由 svc_audio.c 的消费者线程主动调用 DrvAudioOutWrite()
 *    删除弱函数钩子：svc_audio.c 直接持有消息队列，不需要 AODataExport/Free 中转
 *    SetupAudioOutputArgument 弱函数 → 内部 static setup_params()（参数不变）
 *    atomic_int AudioVolume → pthread_mutex_t + int volume
 *
 *
 * ===================== 参数来源 =====================
 *
 *  原版通过 AudioParam.c + #include "ak_common_audio.h" 定义，
 *  编译时 make.sh 调用 add_audio_param_file() 将其替换为实际参数。
 *
 *  为避免弱函数和外部文件依赖，这里将参数直接内联在 setup_params()，
 *  值与原版 ak_audio_config.h 中 default_ao_* 完全一致。
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
    static unsigned char tmp[PCM_VOLUME_BUF_MAX];
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

    /* EQ（default_ao_eq_attr*/
    struct ak_audio_eq_attr eq = {
        1024,
        5,
        {1000, 4500, 4200, 1000, 1250, 0, 0, 0, 0, 0},
        {-2048, -8192, -16384, 0, 0, 0, 0, 0, 0, 0},
        {716, 614, 614, 716, 716, 0, 0, 0, 0, 0},
        {TYPE_HPF, TYPE_PF1, TYPE_HSF, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1},
        0,
        0,
        0,
        0,
        0,
        0,
        1,
        {1, 1, 1, 0, 0, 0, 0, 0, 0, 0}
    };
    ak_ao_set_eq_attr(handle_id, &eq);

    /* 增益（default_ao_gain = 2）*/
    ak_ao_set_gain(handle_id, 2);

    /* AO 音量等级*/
    ak_ao_set_volume(handle_id, 3);

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

 
    s_ao.param.dev_id                    = DEV_ADC;
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
 *   pcm_volume_scale(data, len, volume);  
 *   ak_ao_send_frame(handle, data, len, NULL);
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

    /* 写入 AK AO 硬件*/
    return ak_ao_send_frame(s_ao.handle_id, data, (int)len, NULL);
}

/**
 * @brief 设置音量
 */
void DrvAudioOutSetVolume(int volume)
{

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
