/**
 * @file    drv_audio_in.c
 * @brief   音频采集驱动（互斥锁版）
 *
 * 所有状态集中封装在 DrvAudioInCtx 中，通过 s_ai 单例访问。
 * 采集线程常驻，采集可动态开关（对讲时开，空闲时关，省电）。
 */
#define LOG_TAG "AudioIn"
#include "log.h"

#include "drv_audio_in.h"
#include "ak_ai.h"
#include "ak_common_audio.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* =========================================================
 *  模块状态结构体
 * ========================================================= */
typedef struct {
    pthread_mutex_t          lock;
    AudioInFrameCb           callback;
    int                      capturing;
    int                      running;
    int                      handle_id;
    struct ak_audio_in_param param;
} DrvAudioInCtx;

static DrvAudioInCtx s_ai = {
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .callback  = NULL,
    .capturing = 0,
    .running   = 0,
    .handle_id = -1,
    .param = {
        .dev_id                    = DEV_ADC,
        .pcm_data_attr.sample_rate = AK_AUDIO_SAMPLE_RATE_16000,
        .pcm_data_attr.channel_num = AUDIO_CHANNEL_MONO,
        .pcm_data_attr.sample_bits = AK_AUDIO_SMPLE_BIT_16,
    },
};

/* =========================================================
 *  内部辅助
 * ========================================================= */
static void setup_params(int handle_id)
{
    struct ak_audio_nr_attr   nr   = {-25, 0, 1};
    struct ak_audio_agc_attr  agc  = {16384, 6, 0, 40, 0, 1};
    struct ak_audio_aec_attr  aec  = {0, 1024, 1024, 0, 512, 1, 6553};
    struct ak_audio_aslc_attr aslc = {26214, 10, 0};
    struct ak_audio_eq_attr   eq   = {
        0, 0,
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1,
         TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1},
        0, 0, 0, 0, 0, 0, 0,
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    ak_ai_set_nr_attr(handle_id, &nr);
    ak_ai_set_agc_attr(handle_id, &agc);
    ak_ai_set_aec_attr(handle_id, &aec);
    ak_ai_set_aslc_attr(handle_id, &aslc);
    ak_ai_set_eq_attr(handle_id, &eq);
    ak_ai_set_gain(handle_id, 3);
}

static int open_device(void)
{
    if (ak_ai_open(&s_ai.param, &s_ai.handle_id) != 0) {
        LOG_E("ak_ai_open fail");
        return -1;
    }
    setup_params(s_ai.handle_id);
    if (ak_ai_start_capture(s_ai.handle_id) != 0) {
        LOG_E("start_capture fail");
        ak_ai_close(s_ai.handle_id);
        s_ai.handle_id = -1;
        return -1;
    }
    LOG_I("device opened");
    return 0;
}

static void close_device(void)
{
    if (s_ai.handle_id < 0) return;
    ak_ai_stop_capture(s_ai.handle_id);
    ak_ai_close(s_ai.handle_id);
    s_ai.handle_id = -1;
    LOG_I("device closed");
}

/* =========================================================
 *  采集线程
 * ========================================================= */
static void *audio_in_thread(void *arg)
{
    (void)arg;
    struct frame   hw_frame  = {0};
    static AudioInFrame api_frame;
    unsigned long  seq       = 0;
    int            dev_open  = 0;

    while (1) {
        pthread_mutex_lock(&s_ai.lock);
        int            running   = s_ai.running;
        int            capturing = s_ai.capturing;
        AudioInFrameCb cb        = s_ai.callback;
        pthread_mutex_unlock(&s_ai.lock);

        if (!running) break;

        if (capturing && !dev_open) {
            if (open_device() == 0) dev_open = 1;
        } else if (!capturing && dev_open) {
            close_device();
            dev_open = 0;
        }

        if (dev_open && capturing) {
            if (ak_ai_get_frame(s_ai.handle_id, &hw_frame, 1) == 0) {
                if (hw_frame.data && hw_frame.len > 0 && cb) {
                    api_frame.data = hw_frame.data;
                    api_frame.len  = hw_frame.len;
                    api_frame.seq  = seq++;
                    cb(&api_frame);
                }
                ak_ai_release_frame(s_ai.handle_id, &hw_frame);
            }
        } else {
            usleep(10 * 1000);
        }
        usleep(1000);
    }

    if (dev_open) close_device();
    return NULL;
}

/* =========================================================
 *  公开接口
 * ========================================================= */
void DrvAudioInSetCallback(AudioInFrameCb cb)
{
    pthread_mutex_lock(&s_ai.lock);
    s_ai.callback = cb;
    pthread_mutex_unlock(&s_ai.lock);
}

int DrvAudioInInit(void)
{
    pthread_mutex_lock(&s_ai.lock);
    s_ai.running   = 1;
    s_ai.capturing = 0;
    pthread_mutex_unlock(&s_ai.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, audio_in_thread, NULL) != 0) {
        pthread_mutex_lock(&s_ai.lock);
        s_ai.running = 0;
        pthread_mutex_unlock(&s_ai.lock);
        LOG_E("create thread fail");
        return -1;
    }
    pthread_detach(tid);
    LOG_I("init ok");
    return 0;
}

int DrvAudioInStart(void)
{
    pthread_mutex_lock(&s_ai.lock);
    s_ai.capturing = 1;
    pthread_mutex_unlock(&s_ai.lock);
    return 0;
}

int DrvAudioInStop(void)
{
    pthread_mutex_lock(&s_ai.lock);
    s_ai.capturing = 0;
    pthread_mutex_unlock(&s_ai.lock);
    return 0;
}

int DrvAudioInDeinit(void)
{
    pthread_mutex_lock(&s_ai.lock);
    s_ai.running = 0;
    pthread_mutex_unlock(&s_ai.lock);
    return 0;
}
