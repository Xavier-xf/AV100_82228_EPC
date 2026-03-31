/**
 * @file    svc_svp.h
 * @brief   智能视觉平台服务（原 SmartVisionPlatform.c 重写）
 *
 * ===================== 架构说明 =====================
 *
 *  drv_video_in（第三路通道 320×180）
 *    ↓ VideoFrame 回调
 *  SvcSvpFeedFrame()
 *    ↓ 送帧给 SVP 处理线程
 *  svp_process_thread：
 *    ak_svp_process() → AI 人形检测
 *    DrvMotionDetectGetResult() → 运动检测辅助
 *    IoU 计算 → 过滤误报
 *    →  EventBusPublish(EVT_MOTION_DETECTED, &result)
 *
 * ===================== 灵敏度设置 =====================
 *
 *  SvcSvpSetSensitivity(sensitivity) 对应原 SvpMdFiltersSet：
 *    0 = 关闭检测
 *    1 = 低（flt_big=18000, flt_small=8000）
 *    2 = 中（flt_big=10000, flt_small=5000）
 *    3 = 高（flt_big=7000,  flt_small=3500）
 *
 * ===================== 对外事件 =====================
 *
 *  EVT_MOTION_DETECTED：检测到人形/运动时发布
 *    Arg: SvpDetectResult*（total=0 表示仅运动无人形）
 *
 *  TMR_SVP_ACTIVE：检测到目标后激活 3s（原 SVPTimer）
 *    供 svc_network 心跳帧填写 svp_active 标志位
 */
#ifndef _SVC_SVP_H_
#define _SVC_SVP_H_

#include "drv_video_in.h"
#include "ak_svp.h"   /* AK_SVP_RECT_T */

/* =========================================================
 *  SVP 检测结果结构体
 *  EventBus 事件 EVT_MOTION_DETECTED 的参数类型
 *  对应旧版 SmartVisionPlatformCallback(int Total, AK_SVP_RECT_T *Src)
 * ========================================================= */
#define SVP_DETECT_BOX_MAX  10

typedef struct {
    int           total;                       /* 检测到的目标数（0=无目标）*/
    struct {
        unsigned long left;
        unsigned long top;
        unsigned long right;
        unsigned long bottom;
        int           is_face;   /* 0=人体 1=人脸 */
    } boxes[SVP_DETECT_BOX_MAX];
} SvpDetectResult;


/**
 * @brief 送入一帧视频（由 drv_video_in 回调调用，仅处理第三路小分辨率帧）
 *        内部会按 SvpRate 抽帧，不必每帧都处理
 */
void SvcSvpFeedFrame(const VideoFrame *frame, int width, int height);

/**
 * @brief 设置运动检测灵敏度
 * @param sensitivity  0=关闭 1=低 2=中 3=高
 */
void SvcSvpSetSensitivity(int sensitivity);

/**
 * @brief 查询 SVP 是否在激活状态（原 TimerEnablestatus(SVPTimer)）
 * @return 1=激活（近期有检测结果） 0=空闲
 */
int SvcSvpIsActive(void);

/**
 * @brief 初始化 SVP 服务
 * @param trd_chn_attr  第三路视频通道属性（用于获取分辨率）
 * @param dev_id        视频设备 ID（用于运动检测初始化）
 */
/**
 * @param vi_data_type  第三路通道数据类型（来自 ak_vi.h VI_DATA_TYPE_*）
 *   0 = VI_DATA_TYPE_YUV420SP → AK_SVP_IMG_YUV420SP
 *   其他（如 VI_DATA_TYPE_RGB_LINEINTL）→ AK_SVP_IMG_RGB_LI
 */
int SvcSvpInit(int chn_width, int chn_height, int dev_id, int vi_data_type);

/** @brief 反初始化 */
int SvcSvpDeinit(void);

#endif /* _SVC_SVP_H_ */
