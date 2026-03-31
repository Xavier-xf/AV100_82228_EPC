/**
 * @file    drv_motion_detect.h
 * @brief   运动检测 HAL 驱动（原 MotionDetect.c 重写）
 *
 * 职责：
 *   - 封装 AK MD SDK（ak_mrd / ak_vpss_md）
 *   - 通过 pthread_mutex 替换原 ak_thread_mutex
 *   - 通过回调上报运动检测结果，不含业务逻辑
 *   - 弱函数 SvpMdParamInit / MotionDetectCallBack 改为注册式接口
 *
 * 不做：不画框、不发网络包、不触发定时器
 */
#ifndef _DRV_MOTION_DETECT_H_
#define _DRV_MOTION_DETECT_H_

#include "ak_md.h"

#define MD_BOX_MAX  5

/** 运动检测结果 */
typedef struct {
    int result;           /* 1=有运动 0=无运动    */
    int move_box_num;     /* 运动区域数量         */
    struct {
        int left, top, right, bottom;
    } boxes[MD_BOX_MAX];
    long md_sec;          /* 检测时间戳（秒）     */
} MdResult;

/** 运动检测参数 */
typedef struct {
    int dev_id;
    int md_fps;           /* 帧率（从 ak_vpss_get_sensor_fps 获取）*/
    int move_size_min;
    int move_size_max;
    int flt_big;
    int flt_small;
} MdParam;

/** 运动检测触发回调 */
typedef void (*DrvMdCallback)(int dev_id, const MdResult *result);

/**
 * @brief 注册运动检测回调（Init 前调用）
 */
void DrvMotionDetectSetCallback(DrvMdCallback cb);

/**
 * @brief 设置运动检测参数（不调用则使用默认值）
 */
void DrvMotionDetectSetParam(int dev_id, const MdParam *param);

/**
 * @brief 初始化运动检测模块
 * @param dev_id  设备号（通常为 VIDEO_DEV0）
 */
int DrvMotionDetectInit(int dev_id);

/**
 * @brief 启动/停止运动检测
 * @param dev_id  设备号
 * @param enable  1=启动 0=停止
 */
int DrvMotionDetectEnable(int dev_id, int enable);

/**
 * @brief 设置检测区域过滤参数
 */
int DrvMotionDetectFiltersSet(int dev_id, int flt_big, int flt_small);

/**
 * @brief 获取最新检测结果（非阻塞，无结果返回 -1）
 */
int DrvMotionDetectGetResult(int dev_id, MdResult *out);

/**
 * @brief 反初始化
 */
int DrvMotionDetectUninit(int dev_id);

#endif /* _DRV_MOTION_DETECT_H_ */
