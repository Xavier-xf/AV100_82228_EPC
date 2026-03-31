/**
 * @file    svc_svp.c
 * @brief   智能视觉平台服务（原 SmartVisionPlatform.c 重写）
 *
 * 原代码改进：
 *   ① 弱函数 SmartVisionPlatformPrameInit/Callback → 内置默认参数 + EventBus
 *   ② ak_thread_create → pthread_create
 *   ③ SmartVisionPlatformCallback 中的 SetTimer(SVPTimer) → SvcTimerSet(TMR_SVP_ACTIVE)
 *   ④ ak_vi_draw_box → 直接在此层调用（HAL 操作保留，不上传给 app）
 *   ⑤ SvpMdFiltersSet → SvcSvpSetSensitivity（更语义化的接口）
 *   ⑥ compute_iou2 → 内部 static 函数，无变化
 *   ⑦ SvpParam.EnableCallback → 互斥锁保护的 int s_svp.enable
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
#define SVP_CHN   0   /* AK SVP 通道号 */
#define SVP_RATE  3   /* 每 N 帧检测一次 */

/* =========================================================
 *  IoU 计算用 BOX（与原版相同）
 * ========================================================= */
typedef struct { int x1, y1, x2, y2; } SvpBox;

/* =========================================================
 *  灵敏度映射表（原 SvpMdFiltersSet 的参数）
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
#define SVP_FRAME_BUF_SIZE  (320 * 180 * 3)   /* 第三路 RGB 最大帧大小 */

typedef struct {
    pthread_mutex_t  lock;
    int              enable;        /* 检测开关（灵敏度=0时关闭）*/
    int              dev_id;        /* 关联的视频设备 ID */
    int              chn_w;         /* 第三路通道宽度 */
    int              chn_h;         /* 第三路通道高度 */
    int              chn_img_type;  /* AK_SVP_IMG_YUV420SP or AK_SVP_IMG_RGB_LI */
    int              initialized;
    /* 帧队列（只保留最新一帧，SVP 处理速度比采集慢）
     * ★ 重要：SvcSvpFeedFrame 需拷贝帧数据到 frame_buf，
     *   不能只保存指针。因为 drv_video_in 在回调返回后立即
     *   调用 ak_vi_release_frame，指针会失效（野指针→崩溃）。
     *   旧版 SVP 线程自己调用 ak_vi_get_frame，所有权清晰；
     *   新版通过回调解耦后必须在此做数据拷贝。*/
    pthread_mutex_t  frame_lock;
    pthread_cond_t   frame_cond;
    uint8_t          frame_buf[SVP_FRAME_BUF_SIZE]; /* 帧数据拷贝 */
    uint32_t         frame_len;
    uint32_t         frame_phy_addr;  /* 物理地址（SVP 硬件加速必需）*/
    int              frame_ready;
} SvcSvpCtx;

static SvcSvpCtx s_svp = {
    .lock        = PTHREAD_MUTEX_INITIALIZER,
    .enable      = 1,
    .dev_id      = 0,
    .chn_w        = 320,
    .chn_h        = 180,
    .chn_img_type = AK_SVP_IMG_RGB_LI,  /* 默认第三路 RGB_LINEINTL */
    .initialized = 0,
    .frame_lock  = PTHREAD_MUTEX_INITIALIZER,
    .frame_cond  = PTHREAD_COND_INITIALIZER,
    .frame_ready = 0,
};

/* =========================================================
 *  IoU 计算（原 compute_iou2，无变化）
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
 *  发布检测结果（原 SmartVisionPlatformCallback 逻辑）
 *
 *  原版中：
 *    - ak_vi_draw_box()  → 在视频帧上画框（保留，属于视频渲染，在此层调用）
 *    - SetTimer(SVPTimer) → SvcTimerSet(TMR_SVP_ACTIVE, 3000)
 *    - 用户可重定义此函数 → 改为 EventBusPublish
 * ========================================================= */
static void dispatch_detection(int total, const AK_SVP_OUTPUT_T *output)
{
    SvpDetectResult result;
    memset(&result, 0, sizeof(result));
    result.total = (total > SVP_DETECT_BOX_MAX) ? SVP_DETECT_BOX_MAX : total;

    if (total > 0 && output) {
        /* 画框（视频显示，HAL 层操作）*/
        struct ak_vi_box_group box_info;
        memset(&box_info, 0, sizeof(box_info));
        box_info.draw_frame_num = -1;

        pthread_mutex_lock(&s_svp.lock);
        int chn_w = s_svp.chn_w;
        int chn_h = s_svp.chn_h;
        pthread_mutex_unlock(&s_svp.lock);

        for (int i = 0; i < result.total; i++) {
            const AK_SVP_RECT_T *src = &output->target_boxes[i];
            /* 坐标从第三路分辨率映射到主通道分辨率（原 RectMap 逻辑）*/
            box_info.box[i].left   = (int)((long)src->left   * 1920 / chn_w);
            box_info.box[i].top    = (int)((long)src->top    * 1080 / chn_h);
            box_info.box[i].width  = (int)((long)(src->right  - src->left) * 1920 / chn_w);
            box_info.box[i].height = (int)((long)(src->bottom - src->top)  * 1080 / chn_h);
            box_info.box[i].enable    = 1;
            box_info.box[i].color_id  = (int)src->label;
            box_info.box[i].line_width = 2;

            result.boxes[i].left   = src->left;
            result.boxes[i].top    = src->top;
            result.boxes[i].right  = src->right;
            result.boxes[i].bottom = src->bottom;
            result.boxes[i].is_face = (src->label != AK_SVP_HUMAN_SHAPE) ? 1 : 0;
        }
        ak_vi_draw_box(VIDEO_CHN0, &box_info);
    }

    /* 刷新 SVP 活跃定时器（原 SetTimer(SVPTimer, 3000)）*/
    SvcTimerSet(TMR_SVP_ACTIVE, 3000, NULL, NULL);

    /* 发布检测事件（取代弱函数重定义机制）*/
    EventBusPublish(EVT_MOTION_DETECTED, &result, sizeof(result));
}

/* =========================================================
 *  SVP 帧处理线程（原 SmartVisionPlatformThread）
 *
 *  改动：
 *   - ak_vi_get_frame → 从 SvcSvpFeedFrame() 送入（解耦视频采集）
 *   - ak_thread_create → pthread_create
 *   - ak_mem_alloc → malloc
 * ========================================================= */
static void *svp_process_thread(void *arg)
{
    (void)arg;

    /* 配置 SVP 通道属性（原 SmartVisionPlatformPrameInit 的参数）*/
    AK_SVP_CHN_ATTR_T chn_attr = {0};
    chn_attr.target_type                 = AK_SVP_HUMAN_SHAPE_AND_FACE;
    chn_attr.model_type                  = AK_SVP_MODEL_NORMAL;
    chn_attr.threshold.classify_threshold = 700;
    chn_attr.threshold.IoU_threshold      = 3;

    if (ak_svp_create_chn(SVP_CHN, &chn_attr, NULL, SVP_PATH) != AK_SUCCESS) {
        printf("[SvcSvp] create chn fail\n"); return NULL;
    }

    /* 对象过滤器（SVP_FILTER_MD 模式）*/
    void *filter_handle = ak_object_filter_init();
    ak_object_filter_set_frames(filter_handle, 3, 2, 4);
    ak_object_filter_set_distance_enhancement_params(filter_handle, 2, 8, 6);
    ak_object_filter_set_md_level(filter_handle, 2);
    ak_object_filter_set_continous_enhancement_params(filter_handle, 1, 5);
    ak_object_filter_set_false_record_params(filter_handle, 1, 7);

    OBJECT_BOX *obj_boxes = (OBJECT_BOX *)malloc(sizeof(OBJECT_BOX) * 20);
    if (!obj_boxes) { ak_object_filter_destroy(filter_handle); return NULL; }

    printf("[SvcSvp] process thread start\n");

    unsigned long frame_cnt = 0;
    AK_SVP_IMG_INFO_T input;
    memset(&input, 0, sizeof(input));

    /* 从 s_lock 读取通道尺寸 */
    pthread_mutex_lock(&s_svp.lock);
    /* img_type 由 SvcSvpInit 根据实际通道类型传入（对应旧版 SmartVisionPlatformThread）:
     *   Input.img_type = (ViChnAttr->data_type == VI_DATA_TYPE_YUV420SP ?
     *                     AK_SVP_IMG_YUV420SP : AK_SVP_IMG_RGB_LI)
     * 第三路通道配置为 VI_DATA_TYPE_RGB_LINEINTL → AK_SVP_IMG_RGB_LI */
    input.img_type          = (AK_SVP_IMG_TYPE_E)s_svp.chn_img_type;
    input.width             = (unsigned int)s_svp.chn_w;
    input.height            = (unsigned int)s_svp.chn_h;
    input.pos_info.width    = (unsigned int)s_svp.chn_w;
    input.pos_info.height   = (unsigned int)s_svp.chn_h;
    pthread_mutex_unlock(&s_svp.lock);

    /*
     * ★ 恢复旧版独立线程设计：
     *   原版 SmartVisionPlatformThread 自己调用 ak_vi_get_frame(VIDEO_CHN16)，
     *   获取帧后处理，处理完再 ak_vi_release_frame。
     *   这样 vir_addr 和 phy_addr 都指向尚未释放的有效帧内存。
     *
     *   回调+memcpy 方案的根本问题：
     *   - memcpy 只拷贝了虚拟地址的内容到 frame_buf
     *   - frame_buf 是用户空间的 malloc 内存，物理地址不连续
     *   - 保留原始帧的 phy_addr 是非法的：ak_vi_release_frame 之后
     *     物理页被内核回收，phy_addr 指向的物理内存已无效
     *   - ak_svp_process 硬件直接用 DMA 读 phy_addr，读到垃圾数据
     *   - 日志错误：ak_svp_process error: param input->phy_addr = -2113626112
     *
     *   用户空间无法获取 malloc 内存的物理地址（virt_to_phys 是内核接口）。
     *   唯一正确做法：保持帧不释放直到 SVP 处理完毕，即独立线程模式。
     */
    while (1) {
        struct video_input_frame vi_frame;
        memset(&vi_frame, 0, sizeof(vi_frame));

        /* 直接从第三路通道获取帧（对应旧版 ak_vi_get_frame(ViChnAttr->chn_id, &Frame)）
         * VIDEO_CHN16 = 16（第三路 320×180 RGB）*/
        pthread_mutex_lock(&s_svp.lock);
        int enabled = s_svp.enable;
        int dev_id  = s_svp.dev_id;
        pthread_mutex_unlock(&s_svp.lock);

        if (!enabled) { ak_sleep_ms(50); continue; }

        int ret_vi = ak_vi_get_frame(VIDEO_CHN16, &vi_frame);
        if (ret_vi != AK_SUCCESS) {
            ak_sleep_ms(10);
            continue;
        }

        /* 按 SVP_RATE 抽帧（对应旧版 SvpRate != 0 && ++ViCnt % SvpRate == 0）*/
        if (++frame_cnt % SVP_RATE != 0) {
            ak_vi_release_frame(VIDEO_CHN16, &vi_frame);
            ak_sleep_ms(10);
            continue;
        }

        /* 填充 SVP 输入（vir_addr + phy_addr 均来自未释放的帧，地址有效）*/
        input.vir_addr = vi_frame.vi_frame.data;
        input.phy_addr = vi_frame.phyaddr;   /* ★ 对应旧版 Input.phy_addr = Frame.phyaddr */

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

        /* ---- SVP + MD 融合过滤（原核心逻辑）---- */
        MdResult md_result = {0};
        int has_md = (DrvMotionDetectGetResult(dev_id, &md_result) == 0 &&
                      md_result.result > 0);

        memset(obj_boxes, 0, sizeof(OBJECT_BOX) * (size_t)output.total_num);
        int trigger = 0;

        for (int j = 0; j < output.total_num; j++) {
            AK_SVP_RECT_T *src = &output.target_boxes[j];
            obj_boxes[j].left  = (int)src->left;
            obj_boxes[j].top   = (int)src->top;
            obj_boxes[j].right = (int)src->right;
            obj_boxes[j].bottom= (int)src->bottom;
            obj_boxes[j].count = 1;
            obj_boxes[j].md    = 0;

            if (has_md) {
                SvpBox svp_box = {(int)src->left, (int)src->top,
                                  (int)src->right, (int)src->bottom};
                for (int i = 0; i < md_result.move_box_num; i++) {
                    SvpBox md_box = {
                        md_result.boxes[i].left   * s_svp.chn_w / 32,
                        md_result.boxes[i].top    * s_svp.chn_h / 24,
                        md_result.boxes[i].right  * s_svp.chn_w / 32,
                        md_result.boxes[i].bottom * s_svp.chn_h / 24,
                    };
                    /* 修正宽度为0的情况 */
                    if (md_box.x2 == md_box.x1) md_box.x2++;
                    if (md_box.y2 == md_box.y1) md_box.y2++;

                    double iou_svp = 0, iou_md = 0;
                    compute_iou2(md_box, svp_box, &iou_md, &iou_svp);

                    if (iou_svp * 100 >= 7 && iou_md * 100 >= 40) {
                        obj_boxes[j].md = 1;
                        break;
                    }
                }
            }
        }

        /* 对象过滤器判断 */
        if (ak_object_filter_alarm(filter_handle, obj_boxes, output.total_num) != 0)
            trigger = 1;

        if (trigger)
            dispatch_detection(output.total_num, &output);

        ak_svp_release(&output);
         ak_vi_release_frame(VIDEO_CHN16, &vi_frame);  // ★ 加上这行
         ak_sleep_ms(10);
    }

    free(obj_boxes);
    ak_object_filter_destroy(filter_handle);
    ak_svp_destroy_chn(SVP_CHN);
    return NULL;
}

/* =========================================================
 *  运动检测回调（drv_motion_detect 检测到运动时触发）
 *  仅用于更新 MD 结果供 SVP 融合，不直接触发报警
 * ========================================================= */
static void on_motion_detect(int dev_id, const MdResult *result)
{
    (void)dev_id;
    /* MD 结果已通过 DrvMotionDetectGetResult() 在 SVP 线程中读取，
     * 此回调仅作调试输出用 */
    if (result && result->result)
        printf("[SvcSvp] MD triggered, boxes=%d\n", result->move_box_num);
}

/* =========================================================
 *  公开接口
 * ========================================================= */
/* SvcSvpFeedFrame: 保留 API 兼容性，但 SVP 已改为独立线程
 * 直接调用 ak_vi_get_frame(VIDEO_CHN16) 获取帧，此函数为空操作。*/
void SvcSvpFeedFrame(const VideoFrame *frame, int width, int height)
{
    (void)frame; (void)width; (void)height;
    /* no-op: SVP thread self-acquires via ak_vi_get_frame(VIDEO_CHN16) */
}

void SvcSvpSetSensitivity(int sensitivity)
{
    if (sensitivity < 0) sensitivity = 0;
    if (sensitivity > SENSITIVITY_MAX) sensitivity = SENSITIVITY_MAX;

    pthread_mutex_lock(&s_svp.lock);
    int dev = s_svp.dev_id;
    if (sensitivity == 0) {
        s_svp.enable = 0;
    } else {
        s_svp.enable = 1;
    }
    pthread_mutex_unlock(&s_svp.lock);

    if (sensitivity > 0) {
        int big   = s_sensitivity_map[sensitivity].flt_big;
        int small = s_sensitivity_map[sensitivity].flt_small;
        DrvMotionDetectFiltersSet(dev, big, small);
        DrvMotionDetectEnable(dev, 1);
    } else {
        DrvMotionDetectEnable(dev, 0);
    }

    printf("[SvcSvp] sensitivity=%d\n", sensitivity);
}

int SvcSvpIsActive(void)
{
    return SvcTimerActive(TMR_SVP_ACTIVE);
}

/**
 * @param vi_data_type  第三路通道数据类型（VI_DATA_TYPE_*，来自 drv_video_in）
 *                      VI_DATA_TYPE_YUV420SP → AK_SVP_IMG_YUV420SP
 *                      VI_DATA_TYPE_RGB_LINEINTL → AK_SVP_IMG_RGB_LI（默认）
 */
int SvcSvpInit(int chn_width, int chn_height, int dev_id, int vi_data_type)
{
    /* 初始化 AK SVP 引擎 */
    if (ak_svp_init() != AK_SUCCESS) {
        printf("[SvcSvp] ak_svp_init fail\n"); return -1;
    }

    /* 根据通道数据类型（VI_DATA_TYPE_*）选择 SVP 输入图像格式
     * 对应旧版 SmartVisionPlatformThread 中的动态判断 */
    int img_type = (vi_data_type == 0 /* VI_DATA_TYPE_YUV420SP */)
                 ? (int)AK_SVP_IMG_YUV420SP
                 : (int)AK_SVP_IMG_RGB_LI;   /* VI_DATA_TYPE_RGB_LINEINTL */

    pthread_mutex_lock(&s_svp.lock);
    s_svp.chn_w        = chn_width;
    s_svp.chn_h        = chn_height;
    s_svp.dev_id       = dev_id;
    s_svp.enable       = 1;
    s_svp.initialized  = 1;
    s_svp.chn_img_type = img_type;
    pthread_mutex_unlock(&s_svp.lock);

    /* 初始化运动检测（MD 辅助 SVP）*/
    DrvMotionDetectSetCallback(on_motion_detect);
    if (DrvMotionDetectInit(dev_id) == 0) {
        DrvMotionDetectEnable(dev_id, 1);
        printf("[SvcSvp] MD enabled for dev=%d\n", dev_id);
    } else {
        printf("[SvcSvp] MD init fail, SVP only mode\n");
    }

    /* 启动 SVP 处理线程 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, svp_process_thread, NULL) != 0) {
        printf("[SvcSvp] create thread fail\n");
        ak_svp_deinit();
        return -1;
    }
    pthread_detach(tid);

    printf("[SvcSvp] init ok chn=%dx%d dev=%d\n", chn_width, chn_height, dev_id);
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
