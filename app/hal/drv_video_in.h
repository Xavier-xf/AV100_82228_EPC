/**
 * @file    drv_video_in.h
 * @brief   视频采集与编码驱动（原 VideoInput.c 重写）
 *
 * 职责：
 *   - 加载摄像头内核模块（ISP + Sensor）
 *   - 初始化 AK VI + VENC（三路通道：主/子/第三路）
 *   - 采集 → 编码 → 通过回调上报 H264 码流帧
 *   - 支持动态开关采集（节省功耗）
 *   - 支持昼夜模式切换 / IDR 帧请求
 *
 * 不做：不发送网络包，不知道任何对讲业务
 */
#ifndef _DRV_VIDEO_IN_H_
#define _DRV_VIDEO_IN_H_

#include <stdint.h>
#include "ak_common.h"
#include "ak_vi.h"
#include "ak_venc.h"

/* =========================================================
 *  视频通道枚举
 * ========================================================= */
typedef enum {
    VI_CHN_MAIN,
    VI_CHN_SUB,
    VI_CHN_TRD,
    VI_CHN_TOTAL,
} VI_CHN_NUM;

typedef struct {
    int         EnChn;
    VI_CHN_ATTR_EX ChnAttr;
} ViChnAttr;

/* 视频采集+编码参数
 * ★ 编码器参数用 struct venc_param（AK SDK 实际类型名），
 *   不是 struct ak_venc_param（该类型不存在）*/
typedef struct {
    char              IspPath[128];
    int               DevId;
    ViChnAttr         DevChn[VI_CHN_TOTAL];
    struct venc_param VencParam;   /* 对应 ak_venc.h struct venc_param */
} VideoInputParam;

/* 内核模块路径与名称 */
#define VIDEO_MODULE_PATH       "/usr/modules/"
#define VIDEO_ISP_MODULE_KO     "ak_isp"
#define VIDEO_SENSOR_MODULE_KO  "sensor_gc2083"
#define VIDEO_SENSOR_MODULE_ARG " SENSOR_I2C_ADDR=0x37"
#ifndef ISP_PATH
#define ISP_PATH "/app/isp_gc2083_mipi_2lane_av100.conf"
#endif

/* =========================================================
 *  视频帧数据
 *
 *  ★ phy_addr：AK SVP 硬件加速所需的物理地址。
 *    通过回调传递，必须携带此字段，否则 SVP 硬件无法工作。
 * ========================================================= */
typedef struct {
    uint8_t    *data;        /* H264/YUV/RGB 数据                */
    uint32_t    len;         /* 数据长度（字节）                  */
    uint64_t    pts_ms;      /* 时间戳（毫秒）                   */
    uint32_t    frame_idx;   /* 帧序号                          */
    int         is_idr;      /* 1=IDR 关键帧                    */
    uint32_t    phy_addr;    /* 物理地址（SVP 硬件加速必需）      */
} VideoFrame;

/** H264 帧回调，由采集线程调用，禁止阻塞 */
typedef void (*DrvVideoInFrameCb)(const VideoFrame *frame);

/* =========================================================
 *  接口
 * ========================================================= */

/** 注册 H264 帧回调（Init 前调用）*/
void DrvVideoInSetCallback(DrvVideoInFrameCb cb);

/** 初始化视频采集（加载驱动、打开设备、启动编码线程）*/
int  DrvVideoInInit(void);

/** 开始采集编码（建立对讲/监控时调用）*/
int  DrvVideoInStart(void);

/** 停止采集编码 */
int  DrvVideoInStop(void);

/** 请求编码器输出 IDR 关键帧（室内机请求时调用）*/
void DrvVideoInRequestIdr(void);

/** 切换昼夜模式（0=白天 1=夜间）*/
void DrvVideoInSwitchMode(int night_mode);

/** 反初始化 */
int  DrvVideoInDeinit(void);

#endif /* _DRV_VIDEO_IN_H_ */

/**
 * @brief 第三路视频帧（小分辨率，用于 SVP 运动检测）回调
 *        独立于主码流回调，仅传递 320×180 帧数据
 */
typedef void (*DrvVideoInTrdFrameCb)(const VideoFrame *frame,
                                     int width, int height);

/** 注册第三路帧回调 */
void DrvVideoInSetTrdCallback(DrvVideoInTrdFrameCb cb);
