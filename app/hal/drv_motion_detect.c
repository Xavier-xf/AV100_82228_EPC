/**
 * @file    drv_motion_detect.c
 * @brief   运动检测 HAL 驱动（原 MotionDetect.c 重写）
 *
 * 原代码改进：
 *   ① ak_thread_mutex    → pthread_mutex_t
 *   ② ak_thread_create   → pthread_create / pthread_detach
 *   ③ ak_thread_sem      → sem_t（POSIX 信号量）
 *   ④ 弱函数 SvpMdParamInit / MotionDetectCallBack → 注册式回调 + 默认参数
 *   ⑤ ThreadStat 普通 int，由互斥锁保护
 */
/* ak_common.h 和 ak_log.h 必须在其他 AK SDK 头文件之前包含，
 * 因为 ak_vpss.h/ak_mrd.h 使用了 MODULE_ID_* 宏（定义在 ak_log.h）*/
#include "ak_common.h"
#include "ak_log.h"
#include "drv_motion_detect.h"
#include "ak_vpss.h"
#include "ak_mrd.h"
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define VIDEO_DEV_MUX   2    /* 最大支持的视频设备数 */
#define MAX_CAP        20    /* MRD 最大检测框 */

/* =========================================================
 *  线程状态
 * ========================================================= */
#define MD_THREAD_EXIT  0
#define MD_THREAD_STOP  1
#define MD_THREAD_RUN   2

/* =========================================================
 *  内部结构（ak_thread_mutex → pthread_mutex_t）
 * ========================================================= */
typedef struct {
    pthread_mutex_t lock;
    MdResult        result;
} MdResultStore;

typedef struct {
    pthread_mutex_t lock;          /* 保护 thread_stat */
    int             thread_stat;   /* MD_THREAD_EXIT/STOP/RUN */
    pthread_t       tid;
    sem_t           sem;           /* 结果就绪信号 */
    int             dev;
    int             md_fps;
    void           *mrd_handle;
} MdCtrl;

static MdResultStore  s_md_result[VIDEO_DEV_MUX];
static MdCtrl         s_md_ctrl[VIDEO_DEV_MUX];
static int            s_inited[VIDEO_DEV_MUX]   = {0};
static MdParam        s_md_param[VIDEO_DEV_MUX];

/* ---- 全局回调（互斥锁保护）---- */
typedef struct {
    pthread_mutex_t cb_lock;
    DrvMdCallback   callback;
} DrvMdCtx;

static DrvMdCtx s_md = {
    .cb_lock  = PTHREAD_MUTEX_INITIALIZER,
    .callback = NULL,
};

void DrvMotionDetectSetCallback(DrvMdCallback cb)
{
    pthread_mutex_lock(&s_md.cb_lock);
    s_md.callback = cb;
    pthread_mutex_unlock(&s_md.cb_lock);
}

/* =========================================================
 *  参数设置（替换弱函数 SvpMdParamInit）
 * ========================================================= */
static void apply_default_param(int dev_id, MdParam *p)
{
    p->dev_id = dev_id;
    p->move_size_min = 2;
    p->move_size_max = 300;
    p->flt_big       = 7000;
    p->flt_small     = 3500;
    ak_vpss_get_sensor_fps(dev_id, &p->md_fps);
    if (p->md_fps <= 0) p->md_fps = 15;
}

void DrvMotionDetectSetParam(int dev_id, const MdParam *param)
{
    if (dev_id >= VIDEO_DEV_MUX || !param) return;
    s_md_param[dev_id] = *param;
}

/* =========================================================
 *  获取 ISP 运动统计数据
 * ========================================================= */
static int md_get_stat(int dev, struct vpss_md_info *md)
{
    int ret = ak_vpss_md_get_stat(dev, md);
    if (ret != 0)
        printf("[DrvMD] get stat fail dev=%d\n", dev);
    return ret;
}

/* =========================================================
 *  运动检测判断（原 MotionDetectCheck）
 * ========================================================= */
static int md_check(MdCtrl *ctrl)
{
    int dev = ctrl->dev;
    struct vpss_md_info md = {{{0}}};
    MRD_RECTANGLE boxes[MAX_CAP] = {{0}};

    if (md_get_stat(dev, &md) != 0) return 0;

    int ret = ak_mrd(ctrl->mrd_handle, md.stat, VPSS_MD_DIMENSION_V_MAX, boxes);
    if (ret <= 0) return 0;

    /* 更新检测结果 */
    pthread_mutex_lock(&s_md_result[dev].lock);
    s_md_result[dev].result.result = 1;
    s_md_result[dev].result.move_box_num = (ret > MD_BOX_MAX) ? MD_BOX_MAX : ret;
    for (int i = 0; i < s_md_result[dev].result.move_box_num; i++) {
        s_md_result[dev].result.boxes[i].left   = boxes[i].left;
        s_md_result[dev].result.boxes[i].top    = boxes[i].top;
        s_md_result[dev].result.boxes[i].right  = boxes[i].right;
        s_md_result[dev].result.boxes[i].bottom = boxes[i].bottom;
    }
    pthread_mutex_unlock(&s_md_result[dev].lock);
    return 1;
}

/* =========================================================
 *  运动检测线程（原 MotionDetectThread）
 *  改动：ak_thread_mutex → pthread_mutex_t
 *        ak_sleep_ms     → usleep
 *        ak_get_ostime   → clock_gettime
 * ========================================================= */
static void *md_thread(void *arg)
{
    int dev = *(int *)arg;
    MdCtrl *ctrl = &s_md_ctrl[dev];
    int move_count = 0;
    long last_md_sec = 0;
    struct timespec now;

    printf("[DrvMD] dev=%d thread start\n", dev);

    while (1) {
        pthread_mutex_lock(&ctrl->lock);
        int stat = ctrl->thread_stat;
        int fps  = ctrl->md_fps;
        pthread_mutex_unlock(&ctrl->lock);

        if (stat == MD_THREAD_EXIT) break;

        int interval_ms = (fps > 0) ? (1000 / fps) : 100;

        if (stat == MD_THREAD_STOP) {
            move_count = 0;
            usleep((unsigned int)(interval_ms * 1000));
            continue;
        }

        /* MD_THREAD_RUN */
        if (md_check(ctrl)) {
            move_count++;
            if (move_count >= 1) {
                /* 触发回调 */
                pthread_mutex_lock(&s_md.cb_lock);
                DrvMdCallback cb = s_md.callback;
                pthread_mutex_unlock(&s_md.cb_lock);

                clock_gettime(CLOCK_REALTIME, &now);

                pthread_mutex_lock(&s_md_result[dev].lock);
                s_md_result[dev].result.md_sec = now.tv_sec;
                MdResult snap = s_md_result[dev].result;
                pthread_mutex_unlock(&s_md_result[dev].lock);

                if (cb) cb(dev, &snap);

                last_md_sec = now.tv_sec;
                move_count = 0;
                sem_post(&ctrl->sem);
            }
        } else {
            /* 4秒内未取走结果则清除（原逻辑）*/
            clock_gettime(CLOCK_REALTIME, &now);
            pthread_mutex_lock(&s_md_result[dev].lock);
            if (s_md_result[dev].result.result &&
                (now.tv_sec > last_md_sec + 4)) {
                s_md_result[dev].result.result = 0;
            }
            pthread_mutex_unlock(&s_md_result[dev].lock);
            move_count = 0;
        }

        usleep((unsigned int)(interval_ms * 1000));
    }

    printf("[DrvMD] dev=%d thread exit\n", dev);
    return NULL;
}

/* =========================================================
 *  公开接口
 * ========================================================= */
int DrvMotionDetectInit(int dev_id)
{
    if (dev_id >= VIDEO_DEV_MUX) return -1;
    if (s_inited[dev_id]) return 0;

    /* 参数初始化（若外部未设置则用默认值）*/
    if (s_md_param[dev_id].md_fps == 0)
        apply_default_param(dev_id, &s_md_param[dev_id]);

    MdParam *p    = &s_md_param[dev_id];
    MdCtrl  *ctrl = &s_md_ctrl[dev_id];

    memset(ctrl, 0, sizeof(MdCtrl));
    memset(&s_md_result[dev_id], 0, sizeof(MdResultStore));

    pthread_mutex_init(&ctrl->lock,              NULL);
    pthread_mutex_init(&s_md_result[dev_id].lock, NULL);
    sem_init(&ctrl->sem, 0, 0);

    ctrl->mrd_handle = ak_mrd_init();
    if (!ctrl->mrd_handle) {
        printf("[DrvMD] ak_mrd_init fail\n"); return -1;
    }

    ctrl->dev       = dev_id;
    ctrl->md_fps    = p->md_fps;
    ctrl->thread_stat = MD_THREAD_STOP;

    /* 配置 MRD 参数 */
    ak_mrd_set_mode(ctrl->mrd_handle, 1);   /* 单帧模式 */
    ak_mrd_set_filters(ctrl->mrd_handle, p->flt_big, p->flt_small);
    ak_mrd_set_motion_region_size(ctrl->mrd_handle, p->move_size_min, p->move_size_max);
    ak_mrd_set_floating_wadding_params(ctrl->mrd_handle, 3, 1, 3, 36, 2, 2);

    if (pthread_create(&ctrl->tid, NULL, md_thread, &ctrl->dev) != 0) {
        printf("[DrvMD] create thread fail\n");
        ak_mrd_destroy(ctrl->mrd_handle);
        return -1;
    }
    pthread_detach(ctrl->tid);

    s_inited[dev_id] = 1;
    printf("[DrvMD] dev=%d init ok fps=%d\n", dev_id, p->md_fps);
    return 0;
}

int DrvMotionDetectEnable(int dev_id, int enable)
{
    if (dev_id >= VIDEO_DEV_MUX || !s_inited[dev_id]) return -1;
    MdCtrl *ctrl = &s_md_ctrl[dev_id];

    pthread_mutex_lock(&ctrl->lock);
    if (enable && ctrl->thread_stat == MD_THREAD_STOP)
        ctrl->thread_stat = MD_THREAD_RUN;
    else if (!enable && ctrl->thread_stat == MD_THREAD_RUN)
        ctrl->thread_stat = MD_THREAD_STOP;
    pthread_mutex_unlock(&ctrl->lock);

    printf("[DrvMD] dev=%d %s\n", dev_id, enable ? "enabled" : "disabled");
    return 0;
}

int DrvMotionDetectFiltersSet(int dev_id, int flt_big, int flt_small)
{
    if (dev_id >= VIDEO_DEV_MUX || !s_inited[dev_id]) return -1;
    ak_mrd_set_filters(s_md_ctrl[dev_id].mrd_handle, flt_big, flt_small);
    return 0;
}

int DrvMotionDetectGetResult(int dev_id, MdResult *out)
{
    if (dev_id >= VIDEO_DEV_MUX || !out) return -1;

    pthread_mutex_lock(&s_md_result[dev_id].lock);
    if (!s_md_result[dev_id].result.result) {
        pthread_mutex_unlock(&s_md_result[dev_id].lock);
        return -1;
    }
    *out = s_md_result[dev_id].result;
    memset(&s_md_result[dev_id].result, 0, sizeof(MdResult));
    pthread_mutex_unlock(&s_md_result[dev_id].lock);
    return 0;
}

int DrvMotionDetectUninit(int dev_id)
{
    if (dev_id >= VIDEO_DEV_MUX || !s_inited[dev_id]) return -1;

    s_inited[dev_id] = 0;

    MdCtrl *ctrl = &s_md_ctrl[dev_id];
    pthread_mutex_lock(&ctrl->lock);
    ctrl->thread_stat = MD_THREAD_EXIT;
    pthread_mutex_unlock(&ctrl->lock);

    sem_post(&ctrl->sem);   /* 唤醒可能阻塞的线程 */
    usleep(200 * 1000);     /* 等待线程退出 */

    ak_mrd_destroy(ctrl->mrd_handle);
    ctrl->mrd_handle = NULL;
    sem_destroy(&ctrl->sem);
    pthread_mutex_destroy(&ctrl->lock);
    pthread_mutex_destroy(&s_md_result[dev_id].lock);

    printf("[DrvMD] dev=%d uninit ok\n", dev_id);
    return 0;
}