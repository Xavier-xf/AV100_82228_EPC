/**
 * @file    svc_svp.c
 * @brief   智能视觉平台服务（SVP + MD 联合检测）
 *
 * 设计说明：
 *   SvcTimerSet(TMR_SVP_ACTIVE) 只在检测到目标时调用，以确保定时器能正常超时。
 *   s_svp.use_md 标志控制检测模式：
 *     MD 初始化成功 → use_md=1 → SVP_FILTER_MD 模式（MD IoU + 对象过滤器）
 *     MD 初始化失败 → use_md=0 → NOLY_SVP 模式（直接上报 SVP 结果）
 *   开机灵敏度由室内机通过 MotionSensitivityEvent(0x63) 下发，约 2s 完成同步。
 */

/* ak_common.h 和 ak_log.h 必须在其他 AK SDK 头文件之前 */
#include "ak_common.h"
#include "ak_log.h"

#define LOG_TAG "SvcSvp"
#include "log.h"

#include "ak_svp.h"
#include "ak_vi.h"
#include "ak_object_filter.h"
#include "ak_mem.h"

#include "svc_svp.h"
#include "svc_timer.h"
#include "svc_network.h"
#include "event_bus.h"
#include "drv_motion_detect.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================================================
 *  SVP 模型路径
 * ========================================================= */
#define SVP_PATH  "/usr/sbin/dt320_param.bin"
#define SVP_CHN   0   /* AK SVP 通道号 */
#define SVP_RATE  3   /* 每 N 帧检测一次 */

/* =========================================================
 *  IoU 计算用 BOX
 * ========================================================= */
typedef struct { int x1, y1, x2, y2; } SvpBox;

/* =========================================================
 *  灵敏度映射表
 * ========================================================= */
static const struct { int flt_big; int flt_small; } s_sensitivity_map[] = {
    {0,     0    },   /* 0 = 关闭 */
    {18000, 8000 },   /* 1 = 低   */
    {10000, 5000 },   /* 2 = 中   */
    {7000,  3500 },   /* 3 = 高   */
};
#define SENSITIVITY_MAX  3

/* =========================================================
 *  内部状态结构体（互斥锁保护）
 * ========================================================= */
typedef struct {
    pthread_mutex_t  lock;
    int              enable;       /* 检测开关 */
    int              use_md;       /* 1=SVP_FILTER_MD  0=NOLY_SVP */
    int              dev_id;       /* 关联的视频设备 ID */
    int              chn_w;        /* 第三路通道宽度 */
    int              chn_h;        /* 第三路通道高度 */
    int              chn_img_type; /* AK_SVP_IMG_YUV420SP or AK_SVP_IMG_RGB_LI */
    int              initialized;
} SvcSvpCtx;

static SvcSvpCtx s_svp = {
    .lock         = PTHREAD_MUTEX_INITIALIZER,
    .enable       = 1,
    .use_md       = 0,   /* Init 时根据 MD 初始化结果设置 */
    .dev_id       = 0,
    .chn_w        = 320,
    .chn_h        = 180,
    .chn_img_type = AK_SVP_IMG_RGB_LI,
    .initialized  = 0,
};

/* =========================================================
 *  线性坐标映射，含安全边界 clamp
 * ========================================================= */
static inline int rect_map(int x, int min_in, int max_in, int min_out, int max_out)
{
    if (x >= max_in) return max_out;
    if (x <= min_in) return min_out;
    return ((x - min_in) * (max_out - min_out)) / (max_in - min_in) + min_out;
}

/* =========================================================
 *  IoU 计算
 * ========================================================= */
static void compute_iou2(SvpBox a, SvpBox b, double *r_iou_a, double *r_iou_b)
{
    int max_x = (a.x1 > b.x1) ? a.x1 : b.x1;
    int max_y = (a.y1 > b.y1) ? a.y1 : b.y1;
    int min_x = (a.x2 < b.x2) ? a.x2 : b.x2;
    int min_y = (a.y2 < b.y2) ? a.y2 : b.y2;

    int iw = (min_x - max_x) > 0 ? (min_x - max_x) : 0;
    int ih = (min_y - max_y) > 0 ? (min_y - max_y) : 0;
    double iou_i = (double)(iw * ih);
    double area_a = (double)((a.x2 - a.x1) * (a.y2 - a.y1));
    double area_b = (double)((b.x2 - b.x1) * (b.y2 - b.y1));

    *r_iou_a = (area_a > 0) ? iou_i / area_a : 0;
    *r_iou_b = (area_b > 0) ? iou_i / area_b : 0;
}

/* =========================================================
 *  dispatch_detection：SVP 检测结果分发
 *
 *  total > 0：刷新定时器、映射坐标、画框、发布事件
 *  total == 0：清除画框、发布空事件
 * ========================================================= */
static void dispatch_detection(int total, const AK_SVP_OUTPUT_T *output)
{
    SvpMotionEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.total = total;

    struct ak_vi_box_group box_info;
    memset(&box_info, 0, sizeof(box_info));
    /* draw_frame_num = 0（清除框）由 memset 保持；有框时循环内逐个设为 -1 */

    if (total > 0 && output) {
        evt.boxes_valid = 1;

        pthread_mutex_lock(&s_svp.lock);
        int chn_w = s_svp.chn_w;
        int chn_h = s_svp.chn_h;
        pthread_mutex_unlock(&s_svp.lock);

        for (int i = 0; i < total; i++) {
            const AK_SVP_RECT_T *src = &output->target_boxes[i];
            /* 坐标从第三路映射到主通道 1920×1080，含安全边界 */
            int x = rect_map((int)src->left,                 0, chn_w, 0, 1920);
            int y = rect_map((int)src->top,                  0, chn_h, 0, 1080);
            int w = rect_map((int)(src->right - src->left),  0, chn_w, 0, 1920);
            int h = rect_map((int)(src->bottom - src->top),  0, chn_h, 0, 1080);

            if (i < (int)(sizeof(box_info.box) / sizeof(box_info.box[0]))) {
                box_info.draw_frame_num = -1;   /* 有框时持续画 */
                box_info.box[i].left       = x;
                box_info.box[i].top        = y;
                box_info.box[i].width      = w;
                box_info.box[i].height     = h;
                box_info.box[i].enable     = 1;
                box_info.box[i].color_id   = (int)src->label;
                box_info.box[i].line_width = 2;
            }

            if (i < 4) {
                evt.boxes[i].x     = x;
                evt.boxes[i].y     = y;
                evt.boxes[i].w     = w;
                evt.boxes[i].h     = h;
                evt.boxes[i].score = (int)src->score;
            }
        }

        /* 仅在有检测结果时刷新定时器，确保定时器能正常超时 */
        SvcTimerSet(TMR_SVP_ACTIVE, 3000, NULL, NULL);

        /* 通知室内机（按需启用）*/
        /* SvcNetworkMotionDetectNotify(); */
    }

    /* 始终画框：total==0 时 box_info 全零 → 清除屏幕上的框 */
    ak_vi_draw_box(VIDEO_CHN0, &box_info);

    EventBusPublish(EVT_SVP_MOTION_DETECTED, &evt, sizeof(evt));
}

/* =========================================================
 *  SVP 帧处理线程
 *
 *  use_md=1：SVP_FILTER_MD 模式（MD IoU 校验 + 对象过滤器）
 *  use_md=0：NOLY_SVP 模式（MD 不可用，直接上报 SVP 结果）
 * ========================================================= */
static void *svp_process_thread(void *arg)
{
    (void)arg;

    /* 配置 SVP 通道属性 */
    AK_SVP_CHN_ATTR_T chn_attr = {0};
    chn_attr.target_type                  = AK_SVP_HUMAN_SHAPE_AND_FACE;
    chn_attr.model_type                   = AK_SVP_MODEL_NORMAL;
    chn_attr.threshold.classify_threshold = 700;
    chn_attr.threshold.IoU_threshold      = 3;

    if (ak_svp_create_chn(SVP_CHN, &chn_attr, NULL, SVP_PATH) != AK_SUCCESS) {
        LOG_E("create chn fail"); return NULL;
    }

    /* 对象过滤器（仅 SVP_FILTER_MD 模式下使用）*/
    void *filter_handle = ak_object_filter_init();
    ak_object_filter_set_frames(filter_handle, 3, 2, 4);
    ak_object_filter_set_distance_enhancement_params(filter_handle, 2, 8, 6);
    ak_object_filter_set_md_level(filter_handle, 2);
    ak_object_filter_set_continous_enhancement_params(filter_handle, 1, 5);
    ak_object_filter_set_false_record_params(filter_handle, 1, 7);

    OBJECT_BOX *obj_boxes = (OBJECT_BOX *)malloc(sizeof(OBJECT_BOX) * OBJECT_CAP);
    if (!obj_boxes) { ak_object_filter_destroy(filter_handle); return NULL; }

    LOG_I("process thread start");

    unsigned long frame_cnt = 0;
    AK_SVP_IMG_INFO_T input;
    memset(&input, 0, sizeof(input));

    pthread_mutex_lock(&s_svp.lock);
    input.img_type       = (AK_SVP_IMG_TYPE_E)s_svp.chn_img_type;
    input.width          = (unsigned int)s_svp.chn_w;
    input.height         = (unsigned int)s_svp.chn_h;
    input.pos_info.width = (unsigned int)s_svp.chn_w;
    input.pos_info.height= (unsigned int)s_svp.chn_h;
    pthread_mutex_unlock(&s_svp.lock);

    while (1) {
        pthread_mutex_lock(&s_svp.lock);
        int enabled = s_svp.enable;
        int dev_id  = s_svp.dev_id;
        int use_md  = s_svp.use_md;
        pthread_mutex_unlock(&s_svp.lock);

        /* enable=0：不上报，线程继续空转 */
        if (!enabled) { ak_sleep_ms(50); continue; }

        struct video_input_frame vi_frame;
        memset(&vi_frame, 0, sizeof(vi_frame));
        int ret_vi = ak_vi_get_frame(VIDEO_CHN16, &vi_frame);
        if (ret_vi != AK_SUCCESS) {
            ak_sleep_ms(10);
            continue;
        }

        /* 每 SVP_RATE 帧检测一次 */
        if (++frame_cnt % SVP_RATE != 0) {
            ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
            ak_sleep_ms(10);
            continue;
        }

        input.vir_addr = vi_frame.vi_frame.data;
        input.phy_addr = vi_frame.phyaddr;   /* DMA 需要物理地址，必须来自未释放的帧 */

        AK_SVP_OUTPUT_T output = {0};
        int ret = ak_svp_process(SVP_CHN, &input, &output);

        if (ret != AK_SUCCESS) {
            ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
            ak_svp_reset_hw();
            continue;
        }

        if (output.total_num <= 0) {
            dispatch_detection(0, NULL);
            ak_svp_release(&output);
            ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
            ak_sleep_ms(10);
            continue;
        }

        /* ---- NOLY_SVP 模式：直接上报 ---- */
        if (!use_md) {
            dispatch_detection(output.total_num, &output);
            ak_svp_release(&output);
            ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
            ak_sleep_ms(10);
            continue;
        }

        /* ---- SVP_FILTER_MD 模式：MD IoU 校验 + 对象过滤器 ---- */
        MdResult md_result = {0};
        int has_md = (DrvMotionDetectGetResult(dev_id, &md_result) == 0 &&
                      md_result.result > 0);

        int actual = (output.total_num > OBJECT_CAP) ? OBJECT_CAP : output.total_num;
        memset(obj_boxes, 0, sizeof(OBJECT_BOX) * OBJECT_CAP);

        for (int j = 0; j < actual; j++) {
            AK_SVP_RECT_T *src = &output.target_boxes[j];
            obj_boxes[j].left   = (int)src->left;
            obj_boxes[j].top    = (int)src->top;
            obj_boxes[j].right  = (int)src->right;
            obj_boxes[j].bottom = (int)src->bottom;
            obj_boxes[j].count  = 1;
            obj_boxes[j].md     = 0;

            if (has_md) {
                SvpBox svp_box = {(int)src->left, (int)src->top,
                                  (int)src->right, (int)src->bottom};
                for (int i = 0; i < md_result.move_box_num; i++) {
                    /* 注意：需先判断 right==left（原始坐标）再+1，再乘比例。
                     * 若先乘再+1，同样输入下结果相差10倍，IoU 严重失真。
                     * 例：right=0,left=0,chn_w=320 →
                     *   正确：(0+1)*320/32 = 10px
                     *   错误写法：0*320/32=0, +1=1px（导致漏报）*/
                    int r = md_result.boxes[i].right;
                    int b = md_result.boxes[i].bottom;
                    if (r == md_result.boxes[i].left) r++;
                    if (b == md_result.boxes[i].top)  b++;
                    SvpBox md_box = {
                        md_result.boxes[i].left * s_svp.chn_w / 32,
                        md_result.boxes[i].top  * s_svp.chn_h / 24,
                        r                       * s_svp.chn_w / 32,
                        b                       * s_svp.chn_h / 24,
                    };

                    double iou_svp = 0, iou_md = 0;
                    compute_iou2(md_box, svp_box, &iou_md, &iou_svp);

                    /* IoU 阈值：svp>=7%，md>=40% */
                    if (iou_svp * 100 >= 7 && iou_md * 100 >= 40) {
                        obj_boxes[j].md = 1;
                        break;
                    }
                }
            }
        }

        /* 对象过滤器告警 → 上报 */
        if (ak_object_filter_alarm(filter_handle, obj_boxes, (unsigned int)actual) != 0)
            dispatch_detection(actual, &output);
        ak_svp_release(&output);
        ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
        ak_sleep_ms(10);
    }

    free(obj_boxes);
    ak_object_filter_destroy(filter_handle);
    ak_svp_destroy_chn(SVP_CHN);
    return NULL;
}

/* =========================================================
 *  运动检测回调（MD 线程触发，仅调试输出）
 *  实际结果通过 DrvMotionDetectGetResult() 在 SVP 线程中轮询获取
 * ========================================================= */
static void on_motion_detect(int dev_id, const MdResult *result)
{
    (void)dev_id;
    (void)result;
    /* MD 结果由 SVP 线程通过 DrvMotionDetectGetResult 轮询获取，此回调仅占位 */
}

/* =========================================================
 *  公开接口
 * ========================================================= */

/* SVP 线程已直接调用 ak_vi_get_frame，此接口保留备用 */
void SvcSvpFeedFrame(const VideoFrame *frame, int width, int height)
{
    (void)frame; (void)width; (void)height;
}

/* sensitivity=0 → 关闭上报（不停止 MD 线程，恢复时无需重新初始化）
 * sensitivity>0 → 开启上报，更新 MD 过滤参数（use_md=1 时）*/
void SvcSvpSetSensitivity(int sensitivity)
{
    if (sensitivity < 0) sensitivity = 0;
    if (sensitivity > SENSITIVITY_MAX) sensitivity = SENSITIVITY_MAX;

    pthread_mutex_lock(&s_svp.lock);
    int dev    = s_svp.dev_id;
    int use_md = s_svp.use_md;
    s_svp.enable = (sensitivity > 0) ? 1 : 0;
    pthread_mutex_unlock(&s_svp.lock);

    if (sensitivity > 0 && use_md) {
        int big   = s_sensitivity_map[sensitivity].flt_big;
        int small = s_sensitivity_map[sensitivity].flt_small;
        DrvMotionDetectFiltersSet(dev, big, small);
        DrvMotionDetectEnable(dev, 1);
    }

    LOG_I("sensitivity=%d (enable=%d use_md=%d)",
          sensitivity, (sensitivity > 0) ? 1 : 0, use_md);
}

int SvcSvpIsActive(void)
{
    return SvcTimerActive(TMR_SVP_ACTIVE);
}

int SvcSvpInit(int chn_width, int chn_height, int dev_id, int vi_data_type)
{
    if (ak_svp_init() != AK_SUCCESS) {
        LOG_E("ak_svp_init fail"); return -1;
    }

    int img_type = (vi_data_type == 0 /* VI_DATA_TYPE_YUV420SP */)
                 ? (int)AK_SVP_IMG_YUV420SP
                 : (int)AK_SVP_IMG_RGB_LI;

    pthread_mutex_lock(&s_svp.lock);
    s_svp.chn_w        = chn_width;
    s_svp.chn_h        = chn_height;
    s_svp.dev_id       = dev_id;
    s_svp.enable       = 1;
    s_svp.chn_img_type = img_type;
    s_svp.initialized  = 1;
    pthread_mutex_unlock(&s_svp.lock);

    /* 初始化 MD，失败时退化到 NOLY_SVP 模式（use_md=0） */
    DrvMotionDetectSetCallback(on_motion_detect);
    if (DrvMotionDetectInit(dev_id) == 0) {
        DrvMotionDetectEnable(dev_id, 1);
        pthread_mutex_lock(&s_svp.lock);
        s_svp.use_md = 1;
        pthread_mutex_unlock(&s_svp.lock);
        LOG_I("MD enabled for dev=%d (SVP_FILTER_MD mode)", dev_id);
    } else {
        /* use_md 保持 0：退化到 NOLY_SVP，直接上报 SVP 结果 */
        LOG_W("MD init fail, NOLY_SVP mode");
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, svp_process_thread, NULL) != 0) {
        LOG_E("create thread fail");
        ak_svp_deinit();
        return -1;
    }
    pthread_detach(tid);

    LOG_I("init ok chn=%dx%d dev=%d use_md=%d",
          chn_width, chn_height, dev_id, s_svp.use_md);
    return 0;
}

int SvcSvpDeinit(void)
{
    pthread_mutex_lock(&s_svp.lock);
    s_svp.initialized = 0;
    s_svp.enable = 0;
    pthread_mutex_unlock(&s_svp.lock);

    DrvMotionDetectUninit(s_svp.dev_id);
    ak_svp_deinit();
    return 0;
}
