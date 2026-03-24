/**
 * @file    svc_voice.c
 * @brief   语音播放服务
 */
#include "svc_voice.h"
#include "svc_audio.h"
#include "drv_audio_out.h"

#include "mad.h"
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#define MQ_KEY_VOICE_REQ   ((key_t)0xA002)
#define VOICE_CACHE_MAX    (4 * 1024)
#define MP3_READ_BUF_SIZE  VOICE_CACHE_MAX

/* 请求消息（存文件路径+参数，不存原始音频数据）*/
typedef struct {
    long         mtype;
    VoiceId      id;
    int          volume;
    VoiceEventCb on_start;
    VoiceEventCb on_end;
    char         file_path[256];
} VoiceReqMsg;

#define VOICE_REQ_BODY_SIZE  (sizeof(VoiceReqMsg) - sizeof(long))

/* ---- 状态（互斥锁保护）---- */
typedef struct {
    pthread_mutex_t lock;
    int             mq_id;
    int             initialized;
    int             busy;   /* 1=正在播放 */
} SvcVoiceCtx;

static SvcVoiceCtx s_voc = {
    .lock        = PTHREAD_MUTEX_INITIALIZER,
    .mq_id       = -1,
    .initialized = 0,
    .busy        = 0,
};

static int  get_mqid(void)    { pthread_mutex_lock(&s_voc.lock); int v=s_voc.mq_id;   pthread_mutex_unlock(&s_voc.lock); return v; }
static int  is_init(void)     { pthread_mutex_lock(&s_voc.lock); int v=s_voc.initialized; pthread_mutex_unlock(&s_voc.lock); return v; }
static void set_busy(int val) { pthread_mutex_lock(&s_voc.lock); s_voc.busy=val; pthread_mutex_unlock(&s_voc.lock); }

/* ---- 语音名称表 ---- */
#define _VOICE_NAME(n) #n,
static const char *s_names[VOICE_TOTAL] = { VOICE_LIST(_VOICE_NAME) };

/* ---- 文件路径解析：优先 .pcm，回退 .mp3 ---- */
static int resolve_path(VoiceId id, char *out, size_t max)
{
    snprintf(out, max, "%s%s.pcm", VOICE_BASE_PATH, s_names[id]);
    if (access(out, F_OK) == 0) return 0;
    snprintf(out, max, "%s%s.mp3", VOICE_BASE_PATH, s_names[id]);
    if (access(out, F_OK) == 0) return 0;
    printf("[SvcVoice] not found: %s\n", s_names[id]);
    return -1;
}

/* =========================================================
 *  PCM 送出
 *  半帧等待 + AO缓冲判断追加等待
 * ========================================================= */
static void push_pcm(const uint8_t *pcm, int len)
{
    if (len <= 0 || !pcm) return;
    /* 帧持续时间(ms) = len / (16000Hz × 2byte) × 1000 */
    int frame_ms = (int)((float)len / (16000.0f * 2.0f) * 1000.0f);

    /* 重试送入队列（队列满时退避）*/
    int retry = 0;
    while (SvcAudioFeed(AUDIO_SRC_VOICE, pcm, (uint32_t)len) < 0 && retry < 10) {
        usleep((unsigned int)(frame_ms * 1000));
        retry++;
    }

    /* 流控：固定半帧等待（避免解码过快淹没AO）*/
    usleep((unsigned int)(frame_ms * 500));

    /* AO缓冲充足时追加一帧等待*/
    if (SvcAudioRemainLen() > len * 3)
        usleep((unsigned int)(frame_ms * 1000));
}

/* =========================================================
 *  PCM 文件解码
 * ========================================================= */
static int decode_pcm(const VoiceReqMsg *req)
{
    if (access(req->file_path, F_OK) != 0) {
        printf("[SvcVoice] PCM not found: %s\n", req->file_path); return -1;
    }
    FILE *fp = fopen(req->file_path, "rb");
    if (!fp) return -1;

    static uint8_t buf[VOICE_CACHE_MAX];
    int n;
    while ((n = (int)fread(buf, 1, sizeof(buf), fp)) > 0)
        push_pcm(buf, n);

    fclose(fp);
    return 0;
}

/* =========================================================
 *  MP3 解码
 * ========================================================= */
typedef struct {
    FILE    *fp;
    uint8_t *read_buf;
    uint8_t  cache[VOICE_CACHE_MAX];
    int      cache_len;
} Mp3Ctx;

static int16_t mad_to_s16(mad_fixed_t s)
{
    s += (1L << (MAD_F_FRACBITS - 16));
    if      (s >=  MAD_F_ONE) s =  MAD_F_ONE - 1;
    else if (s < -MAD_F_ONE)  s = -MAD_F_ONE;
    return (int16_t)(s >> (MAD_F_FRACBITS + 1 - 16));
}

static enum mad_flow mp3_input(void *d, struct mad_stream *stream)
{
    Mp3Ctx *ctx = d;
    unsigned long rem = 0;
    if (stream->next_frame) {
        rem = (unsigned long)(ctx->read_buf + MP3_READ_BUF_SIZE - stream->next_frame);
        memmove(ctx->read_buf, stream->next_frame, rem);
    }
    int n = (int)fread(ctx->read_buf + rem, 1, MP3_READ_BUF_SIZE - rem, ctx->fp);
    if (n <= 0) return MAD_FLOW_STOP;
    mad_stream_buffer(stream, ctx->read_buf, rem + (unsigned long)n);
    return MAD_FLOW_CONTINUE;
}

static void mp3_flush(Mp3Ctx *ctx)
{
    if (ctx->cache_len > 0) { push_pcm(ctx->cache, ctx->cache_len); ctx->cache_len = 0; }
}

static enum mad_flow mp3_output(void *d, const struct mad_header *h, struct mad_pcm *pcm)
{
    (void)h;
    Mp3Ctx *ctx = d;
    /* 强制单声道*/
    const mad_fixed_t *left = pcm->samples[0];
    unsigned int n = pcm->length;
    while (n--) {
        int16_t s = mad_to_s16(*left++);
        ctx->cache[ctx->cache_len++] = (uint8_t)(s & 0xFF);
        ctx->cache[ctx->cache_len++] = (uint8_t)((s >> 8) & 0xFF);
    }
    /* 阈值冲洗*/
    int threshold = VOICE_CACHE_MAX - (VOICE_CACHE_MAX % (1152 * 2));
    if (ctx->cache_len >= threshold) mp3_flush(ctx);
    return MAD_FLOW_CONTINUE;
}

static enum mad_flow mp3_error(void *d, struct mad_stream *s, struct mad_frame *f)
{
    (void)s; (void)f;
    mp3_flush((Mp3Ctx *)d);   /* 冲洗最后一段 */
    return MAD_FLOW_CONTINUE;
}

static int decode_mp3(const VoiceReqMsg *req)
{
    if (access(req->file_path, F_OK) != 0) {
        printf("[SvcVoice] MP3 not found: %s\n", req->file_path); return -1;
    }
    Mp3Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = fopen(req->file_path, "rb");
    if (!ctx.fp) return -1;
    ctx.read_buf = malloc(MP3_READ_BUF_SIZE);
    if (!ctx.read_buf) { fclose(ctx.fp); return -1; }

    struct mad_decoder dec;
    mad_decoder_init(&dec, &ctx, mp3_input, NULL, NULL, mp3_output, mp3_error, NULL);
    mad_decoder_run(&dec, MAD_DECODER_MODE_SYNC);
    mad_decoder_finish(&dec);
    mp3_flush(&ctx);   /* 冲洗最后残留 */

    free(ctx.read_buf);
    fclose(ctx.fp);
    return 0;
}

/* =========================================================
 *  解码分发
 * ========================================================= */
static void decode_and_play(const VoiceReqMsg *req)
{
    const char *ext = strrchr(req->file_path, '.');
    if (ext && strcmp(ext, ".mp3") == 0) decode_mp3(req);
    else                                  decode_pcm(req);
}

/* =========================================================
 *  ★ 解码线程
 * ========================================================= */
static void *voice_decode_thread(void *arg)
{
    (void)arg;
    VoiceReqMsg req;
    printf("[SvcVoice] decode thread start\n");

    while (1) {
        int mqid = get_mqid();
        if (mqid < 0) { usleep(50 * 1000); continue; }

        /* 阻塞等待请求*/
        ssize_t ret = msgrcv(mqid, &req, VOICE_REQ_BODY_SIZE, 1, 0);
        if (ret <= 0) {
            /* EINVAL/EIDRM：消息队列被销毁，等待 */
            if (errno == EINVAL || errno == EIDRM) usleep(100 * 1000);
            continue;
        }

        set_busy(1);

        /* 设置播放音量（*/
        DrvAudioOutSetVolume(req.volume);

        /* 用 SvcAudioFlush 清空已积压的对讲帧，防止语音播放后立刻播出
         * 积压的对讲音频（否则会有滞后的回音/杂音）。
         * 后续对讲帧因 mtype=2 优先级低于 mtype=1(VOICE)，
         * 在语音播放期间会积压但不会插入，语音结束后才会播放。*/
        SvcAudioFlush(AUDIO_SRC_INTERCOM);
        if (req.on_start) req.on_start();

        decode_and_play(&req);

        /* 结束回调*/
        if (req.on_end) req.on_end();

        set_busy(0);
        printf("[SvcVoice] done id=%d\n", req.id);
    }
    return NULL;
}

/* =========================================================
 *  公开接口
 * ========================================================= */
void SvcVoicePlay(VoiceId id, int volume,
                  VoiceEventCb on_start, VoiceEventCb on_end)
{
    assert(id < VOICE_TOTAL);
    if (!is_init()) return;
    int mqid = get_mqid();
    if (mqid < 0) return;

    VoiceReqMsg req;
    memset(&req, 0, sizeof(req));
    if (resolve_path(id, req.file_path, sizeof(req.file_path)) != 0) return;

    req.mtype    = 1;
    req.id       = id;
    req.volume   = volume;
    req.on_start = on_start;
    req.on_end   = on_end;

    if (msgsnd(mqid, &req, VOICE_REQ_BODY_SIZE, IPC_NOWAIT) < 0) {
        if (errno == EAGAIN) printf("[SvcVoice] req queue full, drop id=%d\n", id);
        else perror("[SvcVoice] msgsnd");
    }
}

int SvcVoiceBusy(void)
{
    pthread_mutex_lock(&s_voc.lock);
    int v = s_voc.busy;
    pthread_mutex_unlock(&s_voc.lock);
    return v;
}

int SvcVoiceInit(void)
{
    pthread_mutex_lock(&s_voc.lock);
    if (s_voc.initialized) { pthread_mutex_unlock(&s_voc.lock); return 0; }

    int old = msgget(MQ_KEY_VOICE_REQ, 0600);
    if (old >= 0) { msgctl(old, IPC_RMID, NULL); }

    s_voc.mq_id = msgget(MQ_KEY_VOICE_REQ, IPC_CREAT | IPC_EXCL | 0600);
    if (s_voc.mq_id < 0) {
        perror("[SvcVoice] msgget");
        pthread_mutex_unlock(&s_voc.lock);
        return -1;
    }

    s_voc.initialized = 1;
    s_voc.busy = 0;
    pthread_mutex_unlock(&s_voc.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, voice_decode_thread, NULL) != 0) {
        printf("[SvcVoice] create thread fail\n"); return -1;
    }
    pthread_detach(tid);
    printf("[SvcVoice] init ok (mqid=%d)\n", s_voc.mq_id);
    return 0;
}

int SvcVoiceDeinit(void)
{
    pthread_mutex_lock(&s_voc.lock);
    if (!s_voc.initialized) { pthread_mutex_unlock(&s_voc.lock); return 0; }
    if (s_voc.mq_id >= 0) { msgctl(s_voc.mq_id, IPC_RMID, NULL); s_voc.mq_id = -1; }
    s_voc.initialized = 0;
    pthread_mutex_unlock(&s_voc.lock);
    printf("[SvcVoice] deinit ok\n");
    return 0;
}
