/**
 * @file    svc_audio.c
 * @brief   音频输出服务
 */
#include "svc_audio.h"
#include "svc_timer.h"
#include "drv_audio_out.h"
#include "drv_gpio.h"

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#define MQ_KEY_AUDIO_OUT      ((key_t)0xA001)
#define MQ_MSG_PCM_MAX        4096
#define AMP_OFF_DELAY_MS      3000

/* mtype: 消息类型必须 > 0 */
#define MTYPE_VOICE           1
#define MTYPE_INTERCOM        2

typedef struct {
    long     mtype;
    uint32_t len;
    uint8_t  pcm[MQ_MSG_PCM_MAX];
} AudioOutMsg;

#define MSG_BODY_SIZE  (sizeof(AudioOutMsg) - sizeof(long))

/* ---- 内部状态（全部用互斥锁保护）---- */
typedef struct {
    pthread_mutex_t lock;
    int             mq_id;
    int             initialized;
     int             voice_active;   /* ★ 语音播放中标志 */
} SvcAudioCtx;

static SvcAudioCtx s_aud = {
    .lock        = PTHREAD_MUTEX_INITIALIZER,
    .mq_id       = -1,
    .initialized = 0,
    .voice_active = 0,
};

static int get_mqid(void)
{
    pthread_mutex_lock(&s_aud.lock);
    int id = s_aud.mq_id;
    pthread_mutex_unlock(&s_aud.lock);
    return id;
}

static int is_init(void)
{
    pthread_mutex_lock(&s_aud.lock);
    int v = s_aud.initialized;
    pthread_mutex_unlock(&s_aud.lock);
    return v;
}

/* ---- 消息队列创建 ---- */
static int mq_create(key_t key)
{
    int old = msgget(key, 0600);
    if (old >= 0) {
        msgctl(old, IPC_RMID, NULL);
        printf("[SvcAudio] removed stale MQ 0x%X\n", (unsigned)key);
    }
    int id = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (id < 0) { perror("[SvcAudio] msgget"); return -1; }

    /* 扩大队列容量（约 8 帧，root 权限下生效）*/
    struct msqid_ds qattr;
    if (msgctl(id, IPC_STAT, &qattr) == 0) {
        qattr.msg_qbytes = (unsigned long)(MQ_MSG_PCM_MAX * 8);
        msgctl(id, IPC_SET, &qattr);
    }
    return id;
}

/* ---- 功放延迟关闭回调 ---- */
static void on_amp_off(void *arg)
{
    (void)arg;
    DrvGpioAmpDisable();
    printf("[SvcAudio] amp off\n");
}

/* ---- 消费者线程：优先消费 VOICE 帧 ---- */
static void *audio_output_thread(void *arg)
{
    (void)arg;
    AudioOutMsg msg;
    /* 用于检测"播放结束"：有过数据且队列+AO缓冲均空才触发 restart */
    int  played_since_restart = 0;
    printf("[SvcAudio] output thread start\n");

    while (1) {
        int mqid = get_mqid();
        if (mqid < 0) { usleep(10 * 1000); continue; }

        /* 非阻塞取帧：mtype=-MTYPE_INTERCOM 保证 VOICE(1) 优先于 INTERCOM(2) */
        ssize_t ret = msgrcv(mqid, &msg, MSG_BODY_SIZE, -MTYPE_INTERCOM, IPC_NOWAIT);
        if (ret > 0 && msg.len > 0) {
            played_since_restart = 1;

            DrvGpioAmpEnable();
            if (SvcTimerActive(TMR_AMP_OFF))
                SvcTimerRefresh(TMR_AMP_OFF, AMP_OFF_DELAY_MS);
            else
                SvcTimerSet(TMR_AMP_OFF, AMP_OFF_DELAY_MS, on_amp_off, NULL);

            DrvAudioOutWrite(msg.pcm, msg.len);
        } else {
            /* 队列空：等 AO 硬件缓冲也排空后做轻量级 restart，
             * 防止 AO 长时间打开导致内部缓存积累出错 */
            if (played_since_restart && DrvAudioOutRemainLen() == 0) {
                DrvAudioOutRestart();
                played_since_restart = 0;
                printf("[SvcAudio] ao restarted after playback done\n");
            }
            usleep(5 * 1000);   /* 5 ms，控制轮询频率 */
        }

        /* 溢出检测：缓冲区状态异常时完整重启 AO（close + open）*/
        if (DrvAudioOutIsOverflow()) {
            printf("[SvcAudio] ao overflow, reinit\n");
            DrvAudioOutDeinit();
            DrvAudioOutInit();
            played_since_restart = 0;
        }
    }
    return NULL;
}
/* =========================================================
 *  公开接口
 * ========================================================= */
int SvcAudioFeed(AudioSource src, const uint8_t *data, uint32_t len)
{
    if (!is_init() || !data || len == 0 || src >= AUDIO_SRC_MAX) return -1;

    /* ★ 语音播放中，丢弃对讲帧（对应旧版 CircularListLock 机制）*/
    pthread_mutex_lock(&s_aud.lock);
    int voice_playing = s_aud.voice_active;
    pthread_mutex_unlock(&s_aud.lock);
    if (voice_playing && src == AUDIO_SRC_INTERCOM) return -1;

    int mqid = get_mqid();
    if (mqid < 0) return -1;

    AudioOutMsg msg;
    msg.mtype = (src == AUDIO_SRC_VOICE) ? MTYPE_VOICE : MTYPE_INTERCOM;
    msg.len   = (len > MQ_MSG_PCM_MAX) ? MQ_MSG_PCM_MAX : len;
    memcpy(msg.pcm, data, msg.len);

    if (msgsnd(mqid, &msg, MSG_BODY_SIZE, IPC_NOWAIT) < 0) {
        return -1;
    }
    return 0;
}

void SvcAudioFlush(AudioSource src)
{
    int mqid = get_mqid();
    if (mqid < 0) return;
    int mtype = (src == AUDIO_SRC_VOICE) ? MTYPE_VOICE : MTYPE_INTERCOM;
    AudioOutMsg msg;
    int n = 0;
    while (msgrcv(mqid, &msg, MSG_BODY_SIZE, mtype, IPC_NOWAIT) >= 0) n++;
    if (n > 0) printf("[SvcAudio] flushed %d frames src=%d\n", n, src);
}

void SvcAudioVoiceLock(int lock)
{
    pthread_mutex_lock(&s_aud.lock);
    s_aud.voice_active = lock;
    pthread_mutex_unlock(&s_aud.lock);

    /* 锁定：清空已在 AO 队列中的对讲帧 */
    SvcAudioFlush(AUDIO_SRC_INTERCOM);

}
int SvcAudioRemainLen(void) { return DrvAudioOutRemainLen(); }

int SvcAudioInit(void)
{
    pthread_mutex_lock(&s_aud.lock);
    if (s_aud.initialized) { pthread_mutex_unlock(&s_aud.lock); return 0; }

    s_aud.mq_id = mq_create(MQ_KEY_AUDIO_OUT);
    if (s_aud.mq_id < 0) { pthread_mutex_unlock(&s_aud.lock); return -1; }

    s_aud.initialized = 1;
    pthread_mutex_unlock(&s_aud.lock);

    DrvAudioOutInit();
    DrvGpioAmpDisable();

    pthread_t tid;
    if (pthread_create(&tid, NULL, audio_output_thread, NULL) != 0) {
        printf("[SvcAudio] thread create fail\n");
        return -1;
    }
    pthread_detach(tid);
    printf("[SvcAudio] init ok (mqid=%d)\n", s_aud.mq_id);
    return 0;
}

int SvcAudioDeinit(void)
{
    pthread_mutex_lock(&s_aud.lock);
    if (!s_aud.initialized) { pthread_mutex_unlock(&s_aud.lock); return 0; }
    if (s_aud.mq_id >= 0) { msgctl(s_aud.mq_id, IPC_RMID, NULL); s_aud.mq_id = -1; }
    s_aud.initialized = 0;
    pthread_mutex_unlock(&s_aud.lock);
    DrvGpioAmpDisable();
    DrvAudioOutDeinit();
    printf("[SvcAudio] deinit ok\n");
    return 0;
}
