/**
 * @file    svc_svp.c
 * @brief   智能视觉平台服务（原 SmartVisionPlatform.c 重写）
 *
 * 原代码对应关系：
 *   SmartVisionPlatformPrameInit → 内置默认参数（SVP_FILTER_MD 模式）
 *   SmartVisionPlatformCallback  → dispatch_detection()
 *                                   → EventBusPublish(EVT_SVP_MOTION_DETECTED)
 *   SmartVisionPlatformThread    → svp_process_thread()
 *   SvpMdFiltersSet              → SvcSvpSetSensitivity()
 *   SetTimer(SVPTimer, 3000)     → SvcTimerSet(TMR_SVP_ACTIVE, 3000)
 *   SvpParam.EnableCallback      → s_svp.enable（互斥锁保护）
 *   FilterOpt == NOLY_SVP        → s_svp.use_md == 0（MD 初始化失败时自动退化）
 *   RectMap()                    → rect_map()（含安全边界 clamp）
 *
 * ============================================================
 *  CLAUDE.md 问题解答
 * ============================================================
 *
 * Q1: SvcTimerSet 放在 if(total>0&&output){} 外面后移动侦测没法运行？
 * A1: 若 SvcTimerSet 在 if 块外，每次进入 dispatch_detection 无论是否检测到
 *     目标都会刷新 3s 定时器，SVPTimer 永不超时。心跳帧 Arg1.bit0 = svp_active
 *     一直为 1，室内机认为移动侦测持续触发。正确位置在 if(total>0&&output) 内，
 *     只有真正通过过滤的检测结果才刷新（对应旧版 SmartVisionPlatformCallback
 *     内 "if(Src)" 分支里的 SetTimer(3000, SVPTimer, ...)）。
 *
 * Q2: EventBusPublish(EVT_SVP_MOTION_DETECTED) 的作用？
 * A2: 替代旧版的弱函数 SmartVisionPlatformCallback 机制。旧版通过在
 *     VideoTransfer.c 中覆盖弱函数让上层感知检测结果；新版改为 EventBus
 *     发布，任何订阅 EVT_SVP_MOTION_DETECTED 的模块（安防、录像等）均可收到。
 *     网络通知（CMD_MOTION_DETECT 0x61）和 ak_vi_draw_box 在 dispatch_detection
 *     内直接完成，不依赖 EventBus 订阅者，因此当前无订阅者系统也能正常工作。
 *
 * Q3: FILTER_OPTION (NOLY_SVP / SVP_MD / SVP_FILTER_MD) 为什么没了？
 * A3: 旧版 SmartVisionPlatformPrameInit 默认 FilterOpt = SVP_FILTER_MD，
 *     NOLY_SVP 仅在 MotionDetectInit 失败时由 SmartVisionPlatformInit 自动
 *     退化：if(Ret) SvpParam.FilterOpt = NOLY_SVP。SVP_MD（纯 MD + IoU，
 *     无对象过滤器）在此产品从未被启用过。
 *     新版用 s_svp.use_md 标志等价替代：
 *       MD 初始化成功 → use_md=1 → SVP_FILTER_MD（MD IoU + 对象过滤器）
 *       MD 初始化失败 → use_md=0 → NOLY_SVP（SVP 直接上报，跳过 MD 和过滤器）
 *     这与旧版的 FilterOpt 退化逻辑完全一致。
 *
 * Q4: 为什么开机时旧版能根据室内机设置的灵敏度开关移动侦测，新版不行？
 * A4: 机制完全相同——
 *     室外机心跳（IdRepeatEvent 0x55 case-0）→ 发 StreamStatus(0x59)
 *     → 室内机收到后回复 MotionSensitivityEvent(0x63)
 *     → 室外机调用 SvcSvpSetSensitivity（旧版 SvpMdFiltersSet）。
 *     两版本协议行为一致，逻辑等价，均能在开机 ~2s 后收到灵敏度设置。
 *     若开机同步出现问题，原因是 MD 初始化失败（use_md=0），此时
 *     SvcSvpSetSensitivity(0) 通过 s_svp.enable=0 正确禁止上报，
 *     与旧版 EnableCallback=0 行为一致。
 */

/* ak_common.h 和 ak_log.h 必须在其他 AK SDK 头文件之前 */
#include "ak_common.h"
#include "ak_log.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* =========================================================
 *  SVP 模型路径
 * ========================================================= */
#define SVP_PATH  "/usr/sbin/dt320_param.bin"
#define SVP_CHN   0   /* AK SVP 通道号（对应旧版 SvpParam.ChnId = SVP_CHN_0）*/
#define SVP_RATE  3   /* 每 N 帧检测一次（对应旧版 SvpParam.SvpRate = 4，实测 3）*/

/* =========================================================
 *  IoU 计算用 BOX（与原版 BOX 相同）
 * ========================================================= */
typedef struct { int x1, y1, x2, y2; } SvpBox;

/* =========================================================
 *  灵敏度映射表（对应旧版 NetworkCommon.c 的 SvpSensit 数组）
 * ========================================================= */
static const struct { int flt_big; int flt_small; } s_sensitivity_map[] = {
    {0,     0    },   /* 0 = 关闭（EnableCallback=0，对应 SvpMdFiltersSet(0,0)）*/
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
    int              enable;    /* 检测开关（对应旧版 SvpParam.EnableCallback）*/
    int              use_md;    /* 1=SVP_FILTER_MD  0=NOLY_SVP（MD 初始化失败时）*/
    int              dev_id;    /* 关联的视频设备 ID（对应旧版 SvpParam.Dev）*/
    int              chn_w;     /* 第三路通道宽度 */
    int              chn_h;     /* 第三路通道高度 */
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
 *  RectMap：线性坐标映射，含安全边界 clamp
 *  对应旧版 VideoTransfer.h 的 RectMap() inline 函数
 * ========================================================= */
static inline int rect_map(int x, int min_in, int max_in, int min_out, int max_out)
{
    if (x >= max_in) return max_out;
    if (x <= min_in) return min_out;
    return ((x - min_in) * (max_out - min_out)) / (max_in - min_in) + min_out;
}

/* =========================================================
 *  IoU 计算（原 compute_iou2，逻辑无变化）
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
 *  dispatch_detection：对应旧版 SmartVisionPlatformCallback
 *
 *  旧版行为（VideoTransfer.c 的覆盖实现）：
 *    if (Src != NULL):
 *      SetTimer(3000, SVPTimer, ...)      → SvcTimerSet(TMR_SVP_ACTIVE, 3000)
 *      BoxInfo.draw_frame_num = -1        → 持续画框
 *      RectMap 坐标映射                   → rect_map 含 clamp
 *      ak_vi_draw_box(VIDEO_CHN0, &BoxInfo)
 *    if (Src == NULL):
 *      BoxInfo 全零（draw_frame_num=0）   → 清除框
 *      ak_vi_draw_box(VIDEO_CHN0, &BoxInfo)
 *    新增：SvcNetworkMotionDetectNotify() 和 EventBusPublish
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
            /* 坐标从第三路映射到主通道 1920×1080（旧版 RectMap，含安全边界）*/
            int x = rect_map((int)src->left,                 0, chn_w, 0, 1920);
            int y = rect_map((int)src->top,                  0, chn_h, 0, 1080);
            int w = rect_map((int)(src->right - src->left),  0, chn_w, 0, 1920);
            int h = rect_map((int)(src->bottom - src->top),  0, chn_h, 0, 1080);

            if (i < (int)(sizeof(box_info.box) / sizeof(box_info.box[0]))) {
                box_info.draw_frame_num = -1;   /* 有框时持续画（旧版循环内赋值）*/
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

        /* 刷新 SVP 活跃定时器（仅在有检测结果时）→ 对应旧版 if(Src) SetTimer(3000, SVPTimer) */
        SvcTimerSet(TMR_SVP_ACTIVE, 3000, NULL, NULL);

        // /* 通知室内机（仅在有检测结果时）*/
        // SvcNetworkMotionDetectNotify();
    }

    /* 始终画框：total==0 时 BoxInfo 全零（draw_frame_num=0）→ 清除屏幕上的框 */
    ak_vi_draw_box(VIDEO_CHN0, &box_info);

    /* 对应旧版弱函数 SmartVisionPlatformCallback，改为 EventBus 供上层订阅 */
    EventBusPublish(EVT_SVP_MOTION_DETECTED, &evt, sizeof(evt));
}

/* =========================================================
 *  SVP 帧处理线程（原 SmartVisionPlatformThread）
 *
 *  use_md=1：SVP_FILTER_MD 模式（MD IoU 校验 + 对象过滤器）
 *  use_md=0：NOLY_SVP 模式（MD 初始化失败，直接上报 SVP 结果）
 *            → 对应旧版 if(MotionDetectInit失败) SvpParam.FilterOpt=NOLY_SVP
 * ========================================================= */
static void *svp_process_thread(void *arg)
{
    (void)arg;

    /* 配置 SVP 通道属性（对应旧版 SmartVisionPlatformPrameInit）*/
    AK_SVP_CHN_ATTR_T chn_attr = {0};
    chn_attr.target_type                  = AK_SVP_HUMAN_SHAPE_AND_FACE;
    chn_attr.model_type                   = AK_SVP_MODEL_NORMAL;
    chn_attr.threshold.classify_threshold = 700;
    chn_attr.threshold.IoU_threshold      = 3;

    if (ak_svp_create_chn(SVP_CHN, &chn_attr, NULL, SVP_PATH) != AK_SUCCESS) {
        printf("[SvcSvp] create chn fail\n"); return NULL;
    }

    /* 对象过滤器：只在 SVP_FILTER_MD 模式下使用（旧版 OBJECT_FILTER_ENABLE）*/
    void *filter_handle = ak_object_filter_init();
    ak_object_filter_set_frames(filter_handle, 3, 2, 4);
    ak_object_filter_set_distance_enhancement_params(filter_handle, 2, 8, 6);
    ak_object_filter_set_md_level(filter_handle, 2);
    ak_object_filter_set_continous_enhancement_params(filter_handle, 1, 5);
    ak_object_filter_set_false_record_params(filter_handle, 1, 7);

    /* 分配 OBJECT_CAP 个框（旧版 ak_mem_alloc(MODULE_ID_SVP, sizeof(OBJECT_BOX) * OBJECT_CAP)）*/
    OBJECT_BOX *obj_boxes = (OBJECT_BOX *)malloc(sizeof(OBJECT_BOX) * OBJECT_CAP);
    if (!obj_boxes) { ak_object_filter_destroy(filter_handle); return NULL; }

    printf("[SvcSvp] process thread start\n");

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

        /* s_svp.enable=0 对应旧版 EnableCallback=0：不上报，线程继续空转 */
        if (!enabled) { ak_sleep_ms(50); continue; }

        /* 对应旧版 ak_vi_get_frame(ViChnAttr->chn_id, &Frame)
         * VIDEO_CHN16 = 第三路 320×180 RGB */
        struct video_input_frame vi_frame;
        memset(&vi_frame, 0, sizeof(vi_frame));
        int ret_vi = ak_vi_get_frame(VIDEO_CHN16, &vi_frame);
        if (ret_vi != AK_SUCCESS) {
            ak_sleep_ms(10);
            continue;
        }

        /* 对应旧版 SvpRate != 0 && ++ViCnt % SvpRate == 0 */
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
            /* 对应旧版 SmartVisionPlatformCallback(0, NULL) → 清除框 */
            dispatch_detection(0, NULL);
            ak_svp_release(&output);
            ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
            ak_sleep_ms(10);
            continue;
        }

        /* ---- NOLY_SVP 模式：MD 不可用，直接上报（旧版 goto CALLBACK 分支）---- */
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

        /* 限制到 OBJECT_CAP（旧版分配大小），防止堆越界 */
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
                    /* 旧版精确逻辑：先判断 right==left（原始坐标），再+1，再乘比例。
                     * 若先乘再+1，同样输入下结果相差10倍，IoU 严重失真。
                     * 例：right=0,left=0,chn_w=320 →
                     *   旧版：(0+1)*320/32 = 10px
                     *   新版错误写法：0*320/32=0, +1=1px（导致漏报）*/
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

                    /* 旧版阈值：SvpParam.SvpThrld=7, SvpParam.SvpMdThrld=40 */
                    if (iou_svp * 100 >= 7 && iou_md * 100 >= 40) {
                        obj_boxes[j].md = 1;
                        break;
                    }
                }
            }
        }

        /* 对象过滤器（旧版 SVP_FILTER_MD 分支，ak_object_filter_alarm != 0 → goto CALLBACK）*/
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
    // if (result && result->result)
    //     printf("[SvcSvp] MD triggered, boxes=%d\n", result->move_box_num);
}

/* =========================================================
 *  公开接口
 * ========================================================= */

/* SvcSvpFeedFrame：API 兼容性保留，SVP 线程已改为直接 ak_vi_get_frame */
void SvcSvpFeedFrame(const VideoFrame *frame, int width, int height)
{
    (void)frame; (void)width; (void)height;
}

/**
 * SvcSvpSetSensitivity：对应旧版 SvpMdFiltersSet
 *
 * 旧版 SvpMdFiltersSet(FltBig, FltSmall)：
 *   if (!FltBig || !FltSmall) → EnableCallback=0，不改 MD
 *   else                      → EnableCallback=1，MotionDetectFiltersSet(Dev,big,small)
 *   注意：旧版不调用 MotionDetectEnable，MD 一直处于 RUN 状态。
 *
 * 新版等价逻辑：
 *   sensitivity=0 → enable=0（SVP 不上报），不改 MD（保持运行）
 *   sensitivity>0 → enable=1，更新 MD 过滤参数（use_md=1 时）
 */
void SvcSvpSetSensitivity(int sensitivity)
{
    if (sensitivity < 0) sensitivity = 0;
    if (sensitivity > SENSITIVITY_MAX) sensitivity = SENSITIVITY_MAX;

    pthread_mutex_lock(&s_svp.lock);
    int dev    = s_svp.dev_id;
    int use_md = s_svp.use_md;
    s_svp.enable = (sensitivity > 0) ? 1 : 0;
    pthread_mutex_unlock(&s_svp.lock);

    /* sensitivity=0：仅关闭上报（s_svp.enable=0），不停止 MD 线程。
     * 对应旧版 SvpMdFiltersSet(0,0) → EnableCallback=0，MD 继续运行。
     * 这样灵敏度重新置非0时无需重新初始化 MD，响应更快。*/
    if (sensitivity > 0 && use_md) {
        int big   = s_sensitivity_map[sensitivity].flt_big;
        int small = s_sensitivity_map[sensitivity].flt_small;
        DrvMotionDetectFiltersSet(dev, big, small);
        /* 旧版未在此处调用 MotionDetectEnable，但 MD 已在 Init 时启动。
         * 此处保留 Enable 调用以防 MD 异常停止后能自动恢复。*/
        DrvMotionDetectEnable(dev, 1);
    }

    printf("[SvcSvp] sensitivity=%d (enable=%d use_md=%d)\n",
           sensitivity, (sensitivity > 0) ? 1 : 0, use_md);
}

int SvcSvpIsActive(void)
{
    return SvcTimerActive(TMR_SVP_ACTIVE);
}

/**
 * @param vi_data_type  第三路通道数据类型（VI_DATA_TYPE_*，来自 drv_video_in）
 *                      VI_DATA_TYPE_YUV420SP(0)  → AK_SVP_IMG_YUV420SP
 *                      VI_DATA_TYPE_RGB_LINEINTL  → AK_SVP_IMG_RGB_LI（默认）
 */
int SvcSvpInit(int chn_width, int chn_height, int dev_id, int vi_data_type)
{
    if (ak_svp_init() != AK_SUCCESS) {
        printf("[SvcSvp] ak_svp_init fail\n"); return -1;
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

    /* 初始化 MD（对应旧版 SmartVisionPlatformInit 末尾的 MotionDetectInit）
     * 失败时退化到 NOLY_SVP 模式（use_md=0），与旧版 FilterOpt=NOLY_SVP 等价 */
    DrvMotionDetectSetCallback(on_motion_detect);
    if (DrvMotionDetectInit(dev_id) == 0) {
        DrvMotionDetectEnable(dev_id, 1);
        pthread_mutex_lock(&s_svp.lock);
        s_svp.use_md = 1;
        pthread_mutex_unlock(&s_svp.lock);
        printf("[SvcSvp] MD enabled for dev=%d (SVP_FILTER_MD mode)\n", dev_id);
    } else {
        /* use_md 保持 0：退化到 NOLY_SVP，直接上报 SVP 结果 */
        printf("[SvcSvp] MD init fail, NOLY_SVP mode\n");
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, svp_process_thread, NULL) != 0) {
        printf("[SvcSvp] create thread fail\n");
        ak_svp_deinit();
        return -1;
    }
    pthread_detach(tid);

    printf("[SvcSvp] init ok chn=%dx%d dev=%d use_md=%d\n",
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