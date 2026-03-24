/**
 * @file    svc_voice.c
 * @brief   语音播放服务（消息队列 + 互斥锁）
 *
 * 原代码对应：
 *   VoiceRingPlay.c  → SvcVoicePlay + push_pcm_to_audio + 播放队列
 *   VoiceRingTone.c  → voice_decode_thread + decode_mp3 + decode_pcm
 *   VoiceDecode.h    → 弱函数框架（全部删除，改为直接调用）
 *
 * 改进：
 *   ① VoiceToneList(CircularList) → MQ_KEY_VOICE_REQ 消息队列
 *   ② InfoList 空闲槽位查找 → 删除，请求数据直接存在消息体中
 *   ③ AudioFrameCache → 删除，push_pcm_to_audio 用栈缓冲送帧
 *   ④ CircularListLock/Unlock → 删除，mtype=1 优先级保证语音优先
 *   ⑤ VoiceDecodeFinsh(普通int) → s_voc.busy(互斥锁保护)
 *   ⑥ 弱函数(VoiceInfoImport/VoiceDataExport/VoiceDecodeStart/End) → 全部删除
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
 *  PCM 送出（原 VoiceDataExport）
 *  流控与原版一致：半帧等待 + AO缓冲判断追加等待
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

    /* AO缓冲充足时追加一帧等待（原条件：AO剩余 > 3×帧大小）*/
    if (SvcAudioRemainLen() > len * 3)
        usleep((unsigned int)(frame_ms * 1000));
}

/* =========================================================
 *  PCM 文件解码（原 PcmVoiceDataDecode）
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
 *  MP3 解码（原 Mp3VoiceDataDecode，libmad 回调式）
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
    /* 强制单声道（与原代码 pcm->channels=1 一致）*/
    const mad_fixed_t *left = pcm->samples[0];
    unsigned int n = pcm->length;
    while (n--) {
        int16_t s = mad_to_s16(*left++);
        ctx->cache[ctx->cache_len++] = (uint8_t)(s & 0xFF);
        ctx->cache[ctx->cache_len++] = (uint8_t)((s >> 8) & 0xFF);
    }
    /* 阈值冲洗（原注释：防止数组越界，每帧1152×2字节）*/
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
 *  ★ 解码线程（原 VoiceDecodeThread）
 * ========================================================= */
static void *voice_decode_thread(void *arg)
{
    (void)arg;
    VoiceReqMsg req;
    printf("[SvcVoice] decode thread start\n");

    while (1) {
        int mqid = get_mqid();
        if (mqid < 0) { usleep(50 * 1000); continue; }

        /* 阻塞等待请求（原 CircularListRead 的阻塞语义）*/
        ssize_t ret = msgrcv(mqid, &req, VOICE_REQ_BODY_SIZE, 1, 0);
        if (ret <= 0) {
            /* EINVAL/EIDRM：消息队列被销毁，等待 */
            if (errno == EINVAL || errno == EIDRM) usleep(100 * 1000);
            continue;
        }

        set_busy(1);

        /* 设置播放音量（原 AudioOutputVolumeSet(TmpInfo->Volume)）*/
        DrvAudioOutSetVolume(req.volume);

        /* 对应原版 VoiceDecodeStart → CircularListLock：
         * 旧版通过锁住 AudioOutputList 阻止对讲帧在语音播放期间进入播放队列。
         * 新版用 SvcAudioFlush 清空已积压的对讲帧，防止语音播放后立刻播出
         * 积压的对讲音频（否则会有滞后的回音/杂音）。
         * 后续对讲帧因 mtype=2 优先级低于 mtype=1(VOICE)，
         * 在语音播放期间会积压但不会插入，语音结束后才会播放。*/
        SvcAudioFlush(AUDIO_SRC_INTERCOM);
        if (req.on_start) req.on_start();

        decode_and_play(&req);

        /* 结束回调（原 VoiceDecodeEnd：CircularListUnlock + 调用 End）*/
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
