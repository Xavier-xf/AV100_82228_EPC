/**
 * @file    drv_audio_in.c
 * @brief   音频采集驱动（互斥锁版，无原子操作）
 *
 * ===================== 模块状态结构体规范 =====================
 *
 *  所有状态集中封装在 DrvAudioInCtx 中，通过 s_ai 单例访问。
 *
 *  优点：
 *    ① 搜索 s_ai.running 精确定位到本文件，不会命中其他模块的 s_running
 *    ② 模块状态一目了然（看 struct 定义即可）
 *    ③ 扩展字段只需修改 struct，不需要在文件中到处添加 static 变量
 *
 *  结构体命名约定：
 *    文件名缩写_ctx → DrvAudioInCtx  s_ai
 *    drv_audio_out  → DrvAudioOutCtx s_ao
 *    svc_network    → SvcNetCtx      s_net
 *    svc_timer      → SvcTimerCtx    s_tmr
 */
#include "drv_audio_in.h"
#include "ak_ai.h"
#include "ak_common_audio.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* =========================================================
 *  模块状态结构体（替代散落的多个 static 变量）
 * ========================================================= */
typedef struct {
    pthread_mutex_t lock;        /* 保护以下所有字段 */
    AudioInFrameCb  callback;    /* 帧回调，NULL=未注册 */
    int             capturing;   /* 1=采集中 0=停止 */
    int             running;     /* 线程运行标志 */
    /* AK AI 硬件句柄和参数 */
    int                      handle_id;
    struct ak_audio_in_param param;
} DrvAudioInCtx;

static DrvAudioInCtx s_ai = {
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .callback  = NULL,
    .capturing = 0,
    .running   = 0,
    .handle_id = -1,
    /* AK AI 采集参数初始化 */
    .param = {
        .dev_id                        = DEV_ADC,
        .pcm_data_attr.sample_rate     = AK_AUDIO_SAMPLE_RATE_16000,
        .pcm_data_attr.channel_num     = AUDIO_CHANNEL_MONO,
        .pcm_data_attr.sample_bits     = AK_AUDIO_SMPLE_BIT_16,
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
    struct ak_audio_eq_attr eq = {
        0,
        0,
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1, TYPE_PF1},
        0,
        0,
        0,
        0,
        0,
        0,
        0,
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
        printf("[DrvAudioIn] ak_ai_open fail\n"); return -1;
    }
    setup_params(s_ai.handle_id);
    if (ak_ai_start_capture(s_ai.handle_id) != 0) {
        printf("[DrvAudioIn] start_capture fail\n");
        ak_ai_close(s_ai.handle_id);
        s_ai.handle_id = -1;
        return -1;
    }
    printf("[DrvAudioIn] device opened\n");
    return 0;
}

static void close_device(void)
{
    if (s_ai.handle_id < 0) return;
    ak_ai_stop_capture(s_ai.handle_id);
    ak_ai_close(s_ai.handle_id);
    s_ai.handle_id = -1;
    printf("[DrvAudioIn] device closed\n");
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
        return -1;
    }
    pthread_detach(tid);
    printf("[DrvAudioIn] init ok\n");
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
