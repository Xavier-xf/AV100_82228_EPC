/**
 * @file    svc_svp.c
 * @brief   智能视觉平台服务（对照旧版 SmartVisionPlatform.c + VideoTransfer.c 重写）
 *
 * 旧版对应关系：
 *   SmartVisionPlatformPrameInit  → init_svp_param（内置默认值）
 *   SmartVisionPlatformCallback   → svp_callback（含画框、定时器）
 *   SmartVisionPlatformThread     → svp_process_thread
 *   SvpMdFiltersSet               → SvcSvpSetSensitivity
 *   SVPTimer                      → TMR_SVP_ACTIVE
 *   SvpParam.EnableCallback       → s_svp.enable_callback
 *   SvpParam.FilterOpt            → s_svp.filter_opt（FILTER_OPTION）
 *
 * 关键行为保留（与旧版完全一致）：
 *   1. enable_callback=0 时，线程仍采帧、仍运行 SVP，不触发回调/画框/定时器。
 *      total_num==0 时始终调用 svp_callback(0, NULL)（清除屏幕上的框）。
 *   2. ak_object_filter_alarm 的调用位于
 *      "if (AK_SUCCESS == DrvMotionDetectGetResult(...))" 块内，
 *      与旧版完全一致：MD 未运行时跳过 filter。
 *   3. SvcSvpSetSensitivity 不调用 DrvMotionDetectEnable（旧版 SvpMdFiltersSet 相同）。
 *   4. 不向室内机主动发送 MotionDelectEvent（0x61）；
 *      室内机通过心跳中的 SVP 活跃定时器状态感知（与旧版相同）。
 */
#include "ak_common.h"
#include "ak_log.h"
#include "ak_thread.h"
#include "ak_svp.h"
#include "ak_vi.h"
#include "ak_object_filter.h"
#include "ak_mem.h"

#include "svc_svp.h"
#include "svc_timer.h"
#include "event_bus.h"
#include "drv_motion_detect.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* =========================================================
 *  常量
 * ========================================================= */
#define SVP_PATH  "/usr/sbin/dt320_param.bin"
#define SVP_CHN   0           /* SVP_CHN_0 == 0，与旧版 SvpParam.ChnId = SVP_CHN_0 一致 */
#define SVP_RATE  3           /* 每 N 帧检测一次，旧版线程内 SvpRate=3（非 SvpParam.SvpRate=4）*/

/* IoU 阈值（旧版 SvpParam.SvpThrld=7, SvpParam.SvpMdThrld=40）*/
#define SVP_THRLD    7
#define SVP_MD_THRLD 40

/* =========================================================
 *  SVP 过滤选项枚举（与旧版 FILTER_OPTION 完全一致）
 * ========================================================= */
typedef enum {
    NOLY_SVP,
    SVP_MD,
    SVP_FILTER_MD
} FILTER_OPTION;

/* =========================================================
 *  内部 BOX（旧版 BOX 结构）
 * ========================================================= */
typedef struct { int x1, y1, x2, y2; } SvpBox;

/* =========================================================
 *  灵敏度映射（旧版 NetworkCommon.c SvpSensit 数组）
 * ========================================================= */
static const struct { int flt_big; int flt_small; } s_sensitivity_map[] = {
    {0,     0    },   /* 0 = 关闭（EnableCallback=0）*/
    {18000, 8000 },   /* 1 = 低   */
    {10000, 5000 },   /* 2 = 中   */
    {7000,  3500 },   /* 3 = 高   */
};
#define SENSITIVITY_MAX  3

/* =========================================================
 *  内部状态（互斥锁保护）
 * ========================================================= */
typedef struct {
    ak_mutex_t   lock;
    int          enable_callback; /* 对应旧版 SvpParam.EnableCallback */
    FILTER_OPTION filter_opt;     /* 对应旧版 SvpParam.FilterOpt      */
    int          dev_id;
    int          chn_w;
    int          chn_h;
    int          chn_img_type;    /* AK_SVP_IMG_YUV420SP or AK_SVP_IMG_RGB_LI */
    int          initialized;
} SvcSvpCtx;

static SvcSvpCtx s_svp = {
    .lock           = PTHREAD_MUTEX_INITIALIZER,
    .enable_callback = 1,
    .filter_opt     = NOLY_SVP,   /* Init 时根据 MD 结果设置 */
    .dev_id         = 0,
    .chn_w          = 320,
    .chn_h          = 180,
    .chn_img_type   = AK_SVP_IMG_RGB_LI,
    .initialized    = 0,
};

/* =========================================================
 *  RectMap（旧版 VideoTransfer.h RectMap 内联函数）
 * ========================================================= */
static inline int rect_map(int x, int min_in, int max_in, int min_out, int max_out)
{
    if (x >= max_in) return max_out;
    if (x <= min_in) return min_out;
    return ((x - min_in) * (max_out - min_out)) / (max_in - min_in) + min_out;
}

/* =========================================================
 *  IoU 计算（旧版 compute_iou2，逻辑完全一致）
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
 *  svp_callback：对应旧版 SmartVisionPlatformCallback（VideoTransfer.c 覆盖版）
 *
 *  行为（与旧版完全一致）：
 *    Src != NULL → SetTimer(3000, SVPTimer) + 填充框 + draw_frame_num=-1
 *    Src == NULL → BoxInfo 全零（draw_frame_num=0）→ 清除框
 *    均调用 ak_vi_draw_box(VIDEO_CHN0, &BoxInfo)
 *
 *  不发送 MotionDelectEvent（0x61）——旧版没有此发送，
 *  室内机通过心跳 StreamStatus 中的 TimerEnablestatus(SVPTimer) 感知。
 * ========================================================= */
static void svp_callback(int total, const AK_SVP_RECT_T *src)
{
    struct ak_vi_box_group box_info;
    memset(&box_info, 0, sizeof(box_info));

    if (src && total > 0) {
        /* 刷新 SVP 活跃定时器（旧版 SetTimer(3000, SVPTimer, NULL, NULL)）*/
        SvcTimerSet(TMR_SVP_ACTIVE, 3000, NULL, NULL);

        ak_thread_mutex_lock(&s_svp.lock);
        int chn_w = s_svp.chn_w;
        int chn_h = s_svp.chn_h;
        ak_thread_mutex_unlock(&s_svp.lock);

        for (int j = 0; j < total; j++) {
            box_info.draw_frame_num        = -1;   /* draw forever */
            box_info.box[j].left           = (unsigned short)rect_map((int)src[j].left,            0, chn_w, 0, 1920);
            box_info.box[j].top            = (unsigned short)rect_map((int)src[j].top,             0, chn_h, 0, 1080);
            box_info.box[j].width          = (unsigned short)rect_map((int)(src[j].right - src[j].left),  0, chn_w, 0, 1920);
            box_info.box[j].height         = (unsigned short)rect_map((int)(src[j].bottom - src[j].top),  0, chn_h, 0, 1080);
            box_info.box[j].enable         = 1;
            box_info.box[j].color_id       = (unsigned short)src[j].label;
            box_info.box[j].line_width     = 2;
        }

        /* EventBus 通知（新框架替代弱函数机制，不影响旧版功能）*/
        SvpMotionEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.total = total;
        evt.boxes_valid = 1;
        for (int j = 0; j < total && j < 4; j++) {
            evt.boxes[j].x     = box_info.box[j].left;
            evt.boxes[j].y     = box_info.box[j].top;
            evt.boxes[j].w     = box_info.box[j].width;
            evt.boxes[j].h     = box_info.box[j].height;
            evt.boxes[j].score = (int)src[j].score;
        }
        EventBusPublish(EVT_SVP_MOTION_DETECTED, &evt, sizeof(evt));
    }

    /* 始终画框：src==NULL 时 box_info 全零（draw_frame_num=0）→ 清除框 */
    ak_vi_draw_box(VIDEO_CHN0, &box_info);
}

/* =========================================================
 *  SVP 帧处理线程（旧版 SmartVisionPlatformThread）
 *
 *  与旧版完全一致的行为：
 *    enable_callback=0 不跳过采帧和 SVP 处理（旧版 EnableCallback=0 同）。
 *    total_num==0 时始终调用 svp_callback(0, NULL) 清除框。
 *    total_num>0 时的画框回调仅在 enable_callback=1 时触发。
 *    ak_object_filter_alarm 在 DrvMotionDetectGetResult==AK_SUCCESS 块内调用。
 * ========================================================= */
static void *svp_process_thread(void *arg)
{
    (void)arg;
    ak_thread_set_name("svp_process");

    /* 读取 filter_opt（线程启动时已确定，后续不会改变）*/
    ak_thread_mutex_lock(&s_svp.lock);
    FILTER_OPTION filter_opt  = s_svp.filter_opt;
    int           chn_img_type = s_svp.chn_img_type;
    int           chn_w        = s_svp.chn_w;
    int           chn_h        = s_svp.chn_h;
    ak_thread_mutex_unlock(&s_svp.lock);

    /* 对象过滤器：仅在 SVP_FILTER_MD 模式下使用（旧版 #ifdef OBJECT_FILTER_ENABLE）*/
    void       *filter_handle = NULL;
    OBJECT_BOX *obj_boxes     = NULL;
    if (filter_opt == SVP_FILTER_MD) {
        filter_handle = ak_object_filter_init();
        ak_object_filter_set_frames(filter_handle, 3, 2, 4);
        ak_object_filter_set_distance_enhancement_params(filter_handle, 2, 8, 6);
        ak_object_filter_set_md_level(filter_handle, 2);
        ak_object_filter_set_continous_enhancement_params(filter_handle, 1, 5);
        ak_object_filter_set_false_record_params(filter_handle, 1, 7);
        ak_print_notice(MODULE_ID_SVP, "libfilter version = %s\r\n", ak_object_filter_get_version());

        obj_boxes = (OBJECT_BOX *)ak_mem_alloc(MODULE_ID_SVP, sizeof(OBJECT_BOX) * OBJECT_CAP);
        memset(obj_boxes, 0, sizeof(OBJECT_BOX) * OBJECT_CAP);
    }

    /* SVP 输入参数（固定，不在循环内读取）*/
    AK_SVP_IMG_INFO_T input = {0};
    input.img_type            = (AK_SVP_IMG_TYPE_E)chn_img_type;
    input.width               = (unsigned int)chn_w;
    input.height              = (unsigned int)chn_h;
    input.pos_info.width      = (unsigned int)chn_w;
    input.pos_info.height     = (unsigned int)chn_h;
    input.pos_info.left       = 0;
    input.pos_info.top        = 0;

    unsigned long vi_cnt = 0;

    ak_print_normal(MODULE_ID_SVP, "SVP Process start\n");

    while (1) {
        /* 读取 enable_callback 和 dev_id（可能被 SvcSvpSetSensitivity 修改）*/
        ak_thread_mutex_lock(&s_svp.lock);
        int enable_callback = s_svp.enable_callback;
        int dev_id          = s_svp.dev_id;
        ak_thread_mutex_unlock(&s_svp.lock);

        /* 采帧（旧版：无论 EnableCallback 状态都采帧）*/
        struct video_input_frame vi_frame;
        memset(&vi_frame, 0, sizeof(vi_frame));
        int ret_vi = ak_vi_get_frame(VIDEO_CHN16, &vi_frame);
        if (ret_vi != AK_SUCCESS) {
            ak_print_normal_ex(MODULE_ID_SVP, "get frame failed!\n");
            ak_sleep_ms(10);
            continue;
        }

        /* 帧率控制（旧版 SvpRate=3）*/
        if (SVP_RATE != 0 && ++vi_cnt % SVP_RATE != 0) {
            ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
            ak_sleep_ms(10);
            continue;
        }

        input.vir_addr = vi_frame.vi_frame.data;
        input.phy_addr = vi_frame.phyaddr;

        AK_SVP_OUTPUT_T output = {0};
        int ret = ak_svp_process(SVP_CHN, &input, &output);

        if (ret == AK_SUCCESS) {
            if (output.total_num > 0) {
                /* ---- 有检测目标 ---- */
                int do_callback = 0;

                if (filter_opt == NOLY_SVP) {
                    /* 旧版：goto CALLBACK（直接上报，不经 MD/filter）*/
                    do_callback = 1;

                } else if (filter_opt == SVP_FILTER_MD) {
                    /* 填充 ObjBox（旧版 SVP_FILTER_MD 分支）*/
                    int n = (output.total_num > OBJECT_CAP) ? OBJECT_CAP : output.total_num;
                    for (int i = 0; i < n; i++) {
                        AK_SVP_RECT_T *src = &output.target_boxes[i];
                        obj_boxes[i].left   = (short)src->left;
                        obj_boxes[i].top    = (short)src->top;
                        obj_boxes[i].right  = (short)src->right;
                        obj_boxes[i].bottom = (short)src->bottom;
                        obj_boxes[i].count  = 1;
                        obj_boxes[i].md     = 0;
                    }

                    /* 旧版：if (AK_SUCCESS == MotionDetectGetResult(Dev, &RetInfo, 0)) { ... }
                     * DrvMotionDetectGetResult：MD 运行中无结果时也返回 AK_SUCCESS，
                     * 只有 MD 未初始化/非 RUN 状态才返回 FAILED。*/
                    MdResult md_result = {0};
                    if (AK_SUCCESS == DrvMotionDetectGetResult(dev_id, &md_result)) {
                        /* MD 运行中，有运动结果时进行 IoU 校验 */
                        if (md_result.result > 0) {
                            for (int j = 0; j < n; j++) {
                                AK_SVP_RECT_T *src = &output.target_boxes[j];
                                SvpBox svp = {(int)src->left,  (int)src->top,
                                              (int)src->right, (int)src->bottom};
                                for (int i = 0; i < md_result.move_box_num; i++) {
                                    /* 旧版精确逻辑（先 +1 后乘系数）*/
                                    int r = md_result.boxes[i].right;
                                    int b = md_result.boxes[i].bottom;
                                    if (r == md_result.boxes[i].left) r++;
                                    if (b == md_result.boxes[i].top)  b++;
                                    SvpBox md_box = {
                                        md_result.boxes[i].left * chn_w / 32,
                                        md_result.boxes[i].top  * chn_h / 24,
                                        r * chn_w / 32,
                                        b * chn_h / 24,
                                    };
                                    double iou_md = 0, iou_svp = 0;
                                    compute_iou2(md_box, svp, &iou_md, &iou_svp);
                                    if (iou_svp * 100 >= SVP_THRLD && iou_md * 100 >= SVP_MD_THRLD) {
                                        obj_boxes[j].md = 1;
                                        break;
                                    }
                                }
                            }
                        }

                        /* 对象过滤器（旧版：在 MotionDetectGetResult 成功块内调用）*/
                        if (ak_object_filter_alarm(filter_handle, obj_boxes,
                                                   (unsigned int)n) != 0) {
                            do_callback = 1;
                        }
                    }
                    /* else: MD 未运行 → 跳过 filter，goto RELEASE（旧版相同）*/
                }

                if (do_callback) {
                    /* 旧版 CALLBACK 标签：if (EnableCallback) SmartVisionPlatformCallback(...) */
                    if (enable_callback) {
                        svp_callback(output.total_num, output.target_boxes);
                    }
                }
                /* else: goto RELEASE（不上报）*/

            } else {
                /* total_num == 0：始终清除框（旧版：无条件 SmartVisionPlatformCallback(0, NULL)，
                 * 不受 EnableCallback 控制）*/
                svp_callback(0, NULL);
            }

            /* 旧版 RELEASE 标签 */
            ak_svp_release(&output);

        } else {
            /* SVP 处理失败（旧版相同）*/
            ak_svp_reset_hw();
        }

        ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
        ak_sleep_ms(10);
    }

    /* 线程退出清理（旧版 #ifdef OBJECT_FILTER_ENABLE 块）*/
    if (obj_boxes)
        ak_mem_free(obj_boxes);
    if (filter_handle)
        ak_object_filter_destroy(filter_handle);

    ak_print_normal(MODULE_ID_SVP, "capture finish\n\n");
    ak_thread_exit();
    return NULL;
}

/* =========================================================
 *  运动检测回调（MD 线程触发，仅调试输出）
 * ========================================================= */
static void on_motion_detect(int dev_id, const MdResult *result)
{
    (void)dev_id;
    if (result && result->result)
        ak_print_normal(MODULE_ID_MD, "MD triggered, boxes=%d\n", result->move_box_num);
}

/* =========================================================
 *  公开接口
 * ========================================================= */

/**
 * SvcSvpSetSensitivity：对应旧版 SvpMdFiltersSet(FltBig, FltSmall)
 *
 * 旧版逻辑：
 *   if (!FltBig || !FltSmall) → EnableCallback=0，不调用 MotionDetectEnable，不改 filter
 *   else                      → EnableCallback=1，MotionDetectFiltersSet（不调 Enable）
 */
void SvcSvpSetSensitivity(int sensitivity)
{
    if (sensitivity < 0) sensitivity = 0;
    if (sensitivity > SENSITIVITY_MAX) sensitivity = SENSITIVITY_MAX;

    int big   = s_sensitivity_map[sensitivity].flt_big;
    int small = s_sensitivity_map[sensitivity].flt_small;

    ak_thread_mutex_lock(&s_svp.lock);
    int dev     = s_svp.dev_id;
    int use_md  = (s_svp.filter_opt == SVP_FILTER_MD);
    /* 旧版 SvpMdFiltersSet(!big || !small) → EnableCallback=0；否则 1 */
    s_svp.enable_callback = (!big || !small) ? 0 : 1;
    ak_thread_mutex_unlock(&s_svp.lock);

    /* 旧版：有 big/small 时调 MotionDetectFiltersSet，不调 MotionDetectEnable */
    if (big && small && use_md) {
        DrvMotionDetectFiltersSet(dev, big, small);
    }

    printf("[SvcSvp] sensitivity=%d enable_callback=%d (big=%d small=%d)\n",
           sensitivity, (!big || !small) ? 0 : 1, big, small);
}

int SvcSvpIsActive(void)
{
    return SvcTimerActive(TMR_SVP_ACTIVE);
}

/* SvcSvpFeedFrame：API 兼容性保留（SVP 线程已改为直接 ak_vi_get_frame）*/
void SvcSvpFeedFrame(const VideoFrame *frame, int width, int height)
{
    (void)frame; (void)width; (void)height;
}

/**
 * SvcSvpInit：对应旧版 SmartVisionPlatformInit
 *
 * 初始化顺序：
 *   1. ak_svp_init + ak_svp_create_chn
 *   2. DrvMotionDetectInit → 确定 filter_opt（SVP_FILTER_MD 或 NOLY_SVP）
 *   3. ak_thread_create（filter_opt 已确定，线程启动时读到正确值）
 *
 * 注：旧版先建线程再 MotionDetectInit，但线程读取全局 SvpParam.FilterOpt 会产生竞态。
 * 新版将 MD init 移到线程创建前，消除竞态，功能行为等价。
 */
int SvcSvpInit(int chn_width, int chn_height, int dev_id, int vi_data_type)
{
    /* SVP 通道参数（旧版 SmartVisionPlatformPrameInit）*/
    AK_SVP_CHN_ATTR_T chn_attr = {0};
    chn_attr.target_type                  = AK_SVP_HUMAN_SHAPE_AND_FACE;
    chn_attr.model_type                   = AK_SVP_MODEL_NORMAL;
    chn_attr.threshold.classify_threshold = 700;
    chn_attr.threshold.IoU_threshold      = 3;

    int img_type = (vi_data_type == 0 /* VI_DATA_TYPE_YUV420SP */)
                 ? (int)AK_SVP_IMG_YUV420SP
                 : (int)AK_SVP_IMG_RGB_LI;

    if (ak_svp_init() != AK_SUCCESS) {
        printf("[SvcSvp] ak_svp_init fail\n");
        return -1;
    }
    if (ak_svp_create_chn(SVP_CHN, &chn_attr, NULL, SVP_PATH) != AK_SUCCESS) {
        printf("[SvcSvp] create chn fail\n");
        ak_svp_deinit();
        return -1;
    }

    /* MD 初始化（决定 filter_opt，必须在线程创建前完成）*/
    FILTER_OPTION filter_opt = NOLY_SVP;
    DrvMotionDetectSetCallback(on_motion_detect);
    if (DrvMotionDetectInit(dev_id) == AK_SUCCESS) {
        DrvMotionDetectEnable(dev_id, 1);   /* 旧版 MotionDetectEnable(0, 1) */
        filter_opt = SVP_FILTER_MD;
        printf("[SvcSvp] MD enabled → SVP_FILTER_MD\n");
    } else {
        printf("[SvcSvp] MD init fail → NOLY_SVP\n");
    }

    /* 设置全局状态（线程读取前写好）*/
    ak_thread_mutex_lock(&s_svp.lock);
    s_svp.chn_w           = chn_width;
    s_svp.chn_h           = chn_height;
    s_svp.dev_id          = dev_id;
    s_svp.enable_callback  = 1;
    s_svp.chn_img_type    = img_type;
    s_svp.filter_opt      = filter_opt;
    s_svp.initialized     = 1;
    ak_thread_mutex_unlock(&s_svp.lock);

    /* 创建 SVP 处理线程（filter_opt 已确定）*/
    ak_pthread_t tid;
    if (ak_thread_create(&tid, svp_process_thread, NULL,
                         ANYKA_THREAD_NORMAL_STACK_SIZE, -1) != 0) {
        printf("[SvcSvp] create thread fail\n");
        ak_svp_destroy_chn(SVP_CHN);
        ak_svp_deinit();
        return -1;
    }
    ak_thread_detach(tid);

    printf("[SvcSvp] init ok chn=%dx%d dev=%d filter=%d\n",
           chn_width, chn_height, dev_id, filter_opt);
    return 0;
}

int SvcSvpDeinit(void)
{
    ak_thread_mutex_lock(&s_svp.lock);
    s_svp.initialized    = 0;
    s_svp.enable_callback = 0;
    ak_thread_mutex_unlock(&s_svp.lock);

    DrvMotionDetectUninit(s_svp.dev_id);
    ak_svp_destroy_chn(SVP_CHN);
    ak_svp_deinit();
    return 0;
}
