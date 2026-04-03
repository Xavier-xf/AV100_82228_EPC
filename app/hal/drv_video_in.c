/**
 * @file    drv_video_in.c
 * @brief   视频采集与编码驱动（互斥锁版，无原子操作）
 */
#include "drv_video_in.h"
#include "ak_vi.h"
#include "ak_venc.h"
#include "ak_common.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/prctl.h>
#include <time.h>


#define MAIN_W  1920
#define MAIN_H  1080
#define SUB_W   1280
#define SUB_H    720
#define TRD_W    320
#define TRD_H    180

/* ---- 所有状态用互斥锁保护，不用原子操作 ---- */
typedef struct {
    pthread_mutex_t  lock;
    int              capturing;
    int              running;
    int              req_idr;      /* 请求 IDR 关键帧标志 */
    int              night_mode;   /* 昼夜模式 */
    int              curr_mode;    /* 当前模式快照 */
    int              venc_id;
    DrvVideoInFrameCb    frame_cb;
    VideoInputParam  param;
} DrvVideoInCtx;

static DrvVideoInCtx s_vi = {
    .lock       = PTHREAD_MUTEX_INITIALIZER,
    .capturing  = 0,
    .running    = 0,
    .req_idr    = 0,
    .night_mode = 0,
    .curr_mode  = 0,
    .venc_id    = -1,
    .frame_cb       = NULL,
};

void DrvVideoInSetCallback(DrvVideoInFrameCb cb)
{
    pthread_mutex_lock(&s_vi.lock);
    s_vi.frame_cb = cb;
    pthread_mutex_unlock(&s_vi.lock);
}

static int module_is_loaded(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "lsmod | grep -q '^%s '", name);
    return (system(cmd) == 0);
}

static int module_load(const char *path, const char *name, const char *param)
{
    if (module_is_loaded(name)) return 0;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "insmod %s%s.ko %s", path, name, param ? param : "");
    if (system(cmd) != 0) { printf("[DrvVideoIn] insmod %s fail\n", name); return -1; }
    printf("[DrvVideoIn] module %s loaded\n", name);
    return 0;
}

static void init_param(void)
{
    memset(&s_vi.param, 0, sizeof(s_vi.param));
    snprintf(s_vi.param.IspPath, sizeof(s_vi.param.IspPath), "%s", ISP_PATH);  /* /app/isp_gc2083_mipi_2lane_av100.conf */
    s_vi.param.DevId = VIDEO_DEV0;

    s_vi.param.DevChn[VI_CHN_MAIN].EnChn = 1;
    s_vi.param.DevChn[VI_CHN_MAIN].ChnAttr.chn_id = VIDEO_CHN0;
    s_vi.param.DevChn[VI_CHN_MAIN].ChnAttr.res.width  = MAIN_W;
    s_vi.param.DevChn[VI_CHN_MAIN].ChnAttr.res.height = MAIN_H;
    s_vi.param.DevChn[VI_CHN_MAIN].ChnAttr.frame_depth = 2;
    s_vi.param.DevChn[VI_CHN_MAIN].ChnAttr.data_type   = VI_DATA_TYPE_YUV420SP;

    s_vi.param.DevChn[VI_CHN_SUB].EnChn = 1;
    s_vi.param.DevChn[VI_CHN_SUB].ChnAttr.chn_id = VIDEO_CHN1;
    s_vi.param.DevChn[VI_CHN_SUB].ChnAttr.res.width  = SUB_W;
    s_vi.param.DevChn[VI_CHN_SUB].ChnAttr.res.height = SUB_H;
    s_vi.param.DevChn[VI_CHN_SUB].ChnAttr.frame_depth = 2;
    s_vi.param.DevChn[VI_CHN_SUB].ChnAttr.data_type   = VI_DATA_TYPE_YUV420SP;

    s_vi.param.DevChn[VI_CHN_TRD].EnChn = 1;
    s_vi.param.DevChn[VI_CHN_TRD].ChnAttr.chn_id = VIDEO_CHN16;
    s_vi.param.DevChn[VI_CHN_TRD].ChnAttr.res.width  = TRD_W;
    s_vi.param.DevChn[VI_CHN_TRD].ChnAttr.res.height = TRD_H;
    s_vi.param.DevChn[VI_CHN_TRD].ChnAttr.frame_depth = 2;
    s_vi.param.DevChn[VI_CHN_TRD].ChnAttr.data_type   = VI_DATA_TYPE_RGB_LINEINTL;

    struct venc_param *vp = &s_vi.param.VencParam;
    /* 编码器参数（来源：旧版 VideoTransfer.c VideoInputParamInit）*/
    vp->width        = MAIN_W;
    vp->height       = MAIN_H;
    vp->fps          = 25;
    vp->goplen       = (unsigned short)(25 * 2);   /* fps*2 */
    vp->target_kbps  = 2048;
    vp->max_kbps     = 4096;
    vp->br_mode      = BR_MODE_AVBR;
    vp->minqp        = 25;
    vp->maxqp        = 43;
    vp->initqp       = 99;
    vp->jpeg_qlevel  = JPEG_QLEVEL_DEFAULT;
    vp->chroma_mode  = CHROMA_4_2_0;
    vp->enc_out_type = H264_ENC_TYPE;
    vp->profile      = PROFILE_MAIN;
    /* 以下字段旧版也有设置，新版不可省略 */
    vp->max_picture_size  = 0;
    vp->enc_level         = 50;
    vp->smart_mode        = 0;
    vp->smart_goplen      = 100;
    vp->smart_quality     = 50;
    vp->smart_static_value = 0;
}

static int video_dev_init(void)
{
#define VIFAIL(fn, ...) if ((fn)(__VA_ARGS__) != AK_SUCCESS) { \
    printf("[DrvVideoIn] " #fn " fail\n"); ak_vi_close(s_vi.param.DevId); return -1; }

    VIFAIL(ak_vi_open, s_vi.param.DevId);
    VIFAIL(ak_vi_load_sensor_cfg, s_vi.param.DevId, s_vi.param.IspPath);

    RECTANGLE_S res; VI_DEV_ATTR attr = {0};
    attr.dev_id = s_vi.param.DevId;
    attr.max_width = MAIN_W; attr.max_height = MAIN_H;
    attr.sub_max_width = SUB_W; attr.sub_max_height = SUB_H;
    attr.crop.width = MAIN_W; attr.crop.height = MAIN_H;
    if (ak_vi_get_sensor_resolution(s_vi.param.DevId, &res) == AK_SUCCESS) {
        if (attr.crop.width  > res.width)  attr.crop.width  = res.width;
        if (attr.crop.height > res.height) attr.crop.height = res.height;
    }
    VIFAIL(ak_vi_set_dev_attr, s_vi.param.DevId, &attr);

    for (int i = VI_CHN_MAIN; i <= VI_CHN_TRD; i++) {
        if (!s_vi.param.DevChn[i].EnChn) continue;
        VIFAIL(ak_vi_set_chn_attr_ex, s_vi.param.DevChn[i].ChnAttr.chn_id,
               &s_vi.param.DevChn[i].ChnAttr);
    }
    VIFAIL(ak_vi_enable_dev, s_vi.param.DevId);
    for (int i = VI_CHN_MAIN; i <= VI_CHN_TRD; i++) {
        if (!s_vi.param.DevChn[i].EnChn) continue;
        VIFAIL(ak_vi_enable_chn, s_vi.param.DevChn[i].ChnAttr.chn_id);
    }
    if (ak_venc_open(&s_vi.param.VencParam, &s_vi.venc_id) != AK_SUCCESS) {
        printf("[DrvVideoIn] ak_venc_open fail\n");
        ak_vi_close(s_vi.param.DevId); return -1;
    }
        /* 设置画框颜色表（对应旧版 VideoInputDevInit 的 ak_vi_set_box_color_table 调用）
     * 必须在 ak_vi_draw_box 之前设置，否则 draw_box 画出的框不可见 */
    unsigned int color_table[] = {0xff00ff00};
    ak_vi_set_box_color_table(s_vi.param.DevId, color_table);
    printf("[DrvVideoIn] device init ok\n");
    return 0;
}

static void *video_capture_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "drv_video_in");

    struct video_input_frame vi_frame;
    struct video_stream      stream;
    static uint32_t          frame_idx = 0;

    while (1) {
        pthread_mutex_lock(&s_vi.lock);
        int running   = s_vi.running;
        int capturing = s_vi.capturing;
        DrvVideoInFrameCb cb = s_vi.frame_cb;
        pthread_mutex_unlock(&s_vi.lock);

        if (!running) break;

        /* 对应旧版 VideoInputThread：
         *   if (is_video_input_enable == true) {
         *       ak_vi_get_frame(CHN0); encode; release;
         *   }
         *   ak_sleep_ms(1);
         *
         * 旧版 is_video_input_enable=false 时，不取帧，直接 sleep(1ms) 循环。
         * CHN16（SVP 通道）由独立的 svc_svp 线程自己调用 ak_vi_get_frame 获取，
         * 与 CHN0 是否被消耗无关（各通道有独立缓冲）。*/
        if (!capturing) { ak_sleep_ms(1); continue; }

        memset(&vi_frame, 0, sizeof(vi_frame));
        if (ak_vi_get_frame(VIDEO_CHN0, &vi_frame) != AK_SUCCESS) {
            ak_sleep_ms(1); continue;
        }

        memset(&stream, 0, sizeof(stream));
        if (ak_venc_encode_frame(s_vi.venc_id,
                                  vi_frame.vi_frame.data,
                                  vi_frame.vi_frame.len,
                                  NULL, &stream) == AK_SUCCESS) {
            if (stream.data && stream.len > 0 && cb) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                VideoFrame vf = {
                    .data      = stream.data,
                    .len       = stream.len,
                    .pts_ms    = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000,
                    .frame_idx = ++frame_idx,
                    .is_idr    = 0,
                };
                cb(&vf);
                ak_venc_release_stream(s_vi.venc_id, &stream);
            }
        }
        ak_vi_release_frame(VIDEO_CHN0, &vi_frame);

        /* IDR 请求 */
        pthread_mutex_lock(&s_vi.lock);
        int req_idr  = s_vi.req_idr;
        int new_mode = s_vi.night_mode;
        if (req_idr) s_vi.req_idr = 0;
        pthread_mutex_unlock(&s_vi.lock);

        if (req_idr) ak_venc_request_idr(s_vi.venc_id);

        if (new_mode != s_vi.curr_mode) {
            s_vi.curr_mode = new_mode;
            ak_vi_switch_mode(s_vi.param.DevId, s_vi.curr_mode);
        }

        ak_sleep_ms(1);
    }
    printf("[DrvVideoIn] capture thread exit\n");
    return NULL;
}

int DrvVideoInInit(void)
{
    if (module_load(VIDEO_MODULE_PATH, VIDEO_ISP_MODULE_KO,     NULL)                  != 0) return -1;
    if (module_load(VIDEO_MODULE_PATH, VIDEO_SENSOR_MODULE_KO,  VIDEO_SENSOR_MODULE_ARG) != 0) return -1;
    init_param();
    if (video_dev_init() != 0) return -1;

    pthread_mutex_lock(&s_vi.lock);
    s_vi.running   = 1;
    s_vi.capturing = 0;
    pthread_mutex_unlock(&s_vi.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, video_capture_thread, NULL) != 0) {
        printf("[DrvVideoIn] create thread fail\n"); return -1;
    }
    pthread_detach(tid);
    printf("[DrvVideoIn] init ok\n");
    return 0;
}

int DrvVideoInStart(void)
{
    pthread_mutex_lock(&s_vi.lock); s_vi.capturing = 1; pthread_mutex_unlock(&s_vi.lock); return 0;
}

int DrvVideoInStop(void)
{
    pthread_mutex_lock(&s_vi.lock); s_vi.capturing = 0; pthread_mutex_unlock(&s_vi.lock); return 0;
}

void DrvVideoInRequestIdr(void)
{
    pthread_mutex_lock(&s_vi.lock); s_vi.req_idr = 1; pthread_mutex_unlock(&s_vi.lock);
}

void DrvVideoInSwitchMode(int night)
{
    pthread_mutex_lock(&s_vi.lock); s_vi.night_mode = night; pthread_mutex_unlock(&s_vi.lock);
}

int DrvVideoInDeinit(void)
{
    pthread_mutex_lock(&s_vi.lock); s_vi.running = 0; pthread_mutex_unlock(&s_vi.lock);
    if (s_vi.venc_id >= 0) { ak_venc_close(s_vi.venc_id); s_vi.venc_id = -1; }
    if (s_vi.param.DevId >= 0) ak_vi_close(s_vi.param.DevId);
    return 0;
}
