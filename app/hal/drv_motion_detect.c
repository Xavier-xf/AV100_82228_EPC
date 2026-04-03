/**
 * @file    drv_motion_detect.c
 * @brief   运动检测 HAL 驱动（对照 old/common/SmartVisionPlatform/MotionDetect.c 重写）
 *
 * 与旧版对应关系：
 *   MotionDetectThread        → md_thread
 *   MotionDetectCheck         → md_check
 *   MotionDetectInit          → DrvMotionDetectInit
 *   MotionDetectEnable        → DrvMotionDetectEnable
 *   MotionDetectFiltersSet    → DrvMotionDetectFiltersSet
 *   MotionDetectGetResult     → DrvMotionDetectGetResult
 *   MotionDetectUninit        → DrvMotionDetectUninit
 *   SvpMdParamInit（弱函数）   → apply_default_param（内部默认参数）
 *   MotionDetectCallBack（弱函数）→ DrvMotionDetectSetCallback 注册式替代
 *
 * 关键行为保留（与旧版完全一致）：
 *   DrvMotionDetectGetResult 在 MD 线程 RUN 状态下始终返回 AK_SUCCESS，
 *   即使当前无运动（result=0）——对应旧版 Timeout=0 路径 "return 0"。
 *   这保证 svc_svp.c 中 ak_object_filter_alarm 的正确调用时机。
 */
#include "ak_common.h"
#include "ak_log.h"
#include "ak_thread.h"
#include "drv_motion_detect.h"
#include "ak_vpss.h"
#include "ak_mrd.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define VIDEO_DEV_MUX   2    /* 最大视频设备数 */
#define MAX_CAP        20    /* MRD 最大检测框 */

/* 线程状态（与旧版完全一致）*/
enum {
    MD_THREAD_EXIT = 0,
    MD_THREAD_STOP,
    MD_THREAD_RUN,
};

/* 结果存储（旧版 HwMdResult）*/
struct HwMdResult {
    ak_mutex_t    Lock;
    MdResult      RetInfo;
};

/* 控制结构（旧版 HwMdCtrl）*/
struct HwMdCtrl {
    int            ThreadStat;   /* 直接读写，不额外加锁（旧版相同）*/
    ak_pthread_t   MdTid;
    ak_sem_t       SendSem;
    int            Dev;
    int            MdFps;
    void          *MrdHandle;
};

static struct HwMdResult s_mdResults[VIDEO_DEV_MUX];
static struct HwMdCtrl   MdCtrl[VIDEO_DEV_MUX];
static int               MdInitFlag[VIDEO_DEV_MUX] = {0};
static MdParam           s_md_param[VIDEO_DEV_MUX];

/* ---- 全局回调（替换旧版弱函数 MotionDetectCallBack）---- */
static ak_mutex_t    s_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static DrvMdCallback s_callback = NULL;

void DrvMotionDetectSetCallback(DrvMdCallback cb)
{
    ak_thread_mutex_lock(&s_cb_lock);
    s_callback = cb;
    ak_thread_mutex_unlock(&s_cb_lock);
}

/* =========================================================
 *  参数默认值（替换旧版弱函数 SvpMdParamInit）
 * ========================================================= */
static void apply_default_param(int dev_id, MdParam *p)
{
    p->dev_id       = dev_id;
    p->move_size_min = 1500;
    p->move_size_max = 7000;
    p->flt_big       = 7000;
    p->flt_small     = 3500;
    ak_vpss_get_sensor_fps(dev_id, &p->md_fps);
    if (p->md_fps <= 0) p->md_fps = 15;
}

/* =========================================================
 *  ISP 运动统计数据获取（旧版 MotionDetectGetStat）
 * ========================================================= */
static int md_get_stat(int dev, struct vpss_md_info *md)
{
    int ret = ak_vpss_md_get_stat(dev, md);
    if (ret != 0)
        ak_print_error_ex(MODULE_ID_MD, "get md stat fail dev=%d\n", dev);
    return ret;
}

/* =========================================================
 *  运动检测判断（旧版 MotionDetectCheck）
 *  只填充 boxes/move_box_num，不设置 result=1。
 *  result=1 由 md_thread 在回调后设置（与旧版顺序完全一致）。
 * ========================================================= */
static int md_check(struct HwMdCtrl *ctrl)
{
    int dev = ctrl->Dev;
    struct vpss_md_info md = {{{0}}};
    MRD_RECTANGLE boxes[MAX_CAP] = {{0}};

    if (md_get_stat(dev, &md) != 0)
        return 0;

    int ret = ak_mrd(ctrl->MrdHandle, md.stat, VPSS_MD_DIMENSION_V_MAX, boxes);
    if (ret <= 0)
        return 0;

    ak_thread_mutex_lock(&s_mdResults[dev].Lock);
    memset(&s_mdResults[dev].RetInfo.boxes, 0, MD_BOX_MAX * sizeof(s_mdResults[dev].RetInfo.boxes[0]));
    s_mdResults[dev].RetInfo.move_box_num = (ret > MD_BOX_MAX) ? MD_BOX_MAX : ret;
    for (int i = 0; i < s_mdResults[dev].RetInfo.move_box_num; i++) {
        s_mdResults[dev].RetInfo.boxes[i].left   = boxes[i].left;
        s_mdResults[dev].RetInfo.boxes[i].top    = boxes[i].top;
        s_mdResults[dev].RetInfo.boxes[i].right  = boxes[i].right;
        s_mdResults[dev].RetInfo.boxes[i].bottom = boxes[i].bottom;
    }
    /* result 不在此处设置（旧版 MotionDetectCheck 相同逻辑）*/
    ak_thread_mutex_unlock(&s_mdResults[dev].Lock);
    return 1;
}

/* =========================================================
 *  运动检测线程（旧版 MotionDetectThread）
 * ========================================================= */
static void *md_thread(void *arg)
{
    int dev = *(int *)arg;
    int move_count = 0;
    struct ak_timeval MdTv = {0}, NowTv = {0};
    struct ak_date Date = {0};

    ak_thread_set_name("svp_md");
    ak_print_normal_ex(MODULE_ID_MD, "thread id : %ld\n", ak_thread_get_tid());

    while (MdCtrl[dev].ThreadStat) {
        int detect_interval = 1000 / MdCtrl[dev].MdFps;

        /* 停止模式：等待直到 RUN（旧版 do-while 逻辑）*/
        do {
            ak_sleep_ms(detect_interval);
            if (MD_THREAD_STOP == MdCtrl[dev].ThreadStat)
                move_count = 0;
        } while (MD_THREAD_STOP == MdCtrl[dev].ThreadStat);

        if (md_check(&MdCtrl[dev])) {
            move_count++;
            if (move_count >= 1) {
                /* 步骤1：触发回调（旧版 MotionDetectCallBack）*/
                ak_thread_mutex_lock(&s_cb_lock);
                DrvMdCallback cb = s_callback;
                ak_thread_mutex_unlock(&s_cb_lock);
                if (cb) {
                    MdResult snap = {0};
                    ak_thread_mutex_lock(&s_mdResults[dev].Lock);
                    snap = s_mdResults[dev].RetInfo;
                    ak_thread_mutex_unlock(&s_mdResults[dev].Lock);
                    cb(dev, &snap);
                }

                /* 步骤2：设置时间戳和 result=1（旧版顺序）*/
                ak_get_ostime(&MdTv);
                ak_get_localdate(&Date);
                ak_thread_mutex_lock(&s_mdResults[dev].Lock);
                s_mdResults[dev].RetInfo.md_sec = ak_date_to_seconds(&Date);
                s_mdResults[dev].RetInfo.result = 1;
                ak_thread_mutex_unlock(&s_mdResults[dev].Lock);

                move_count = 0;
                ak_thread_sem_post(&MdCtrl[dev].SendSem);
            }
        } else {
            /* 4秒未取走结果则清除（旧版逻辑）*/
            ak_thread_mutex_lock(&s_mdResults[dev].Lock);
            if (1 == s_mdResults[dev].RetInfo.result) {
                ak_get_ostime(&NowTv);
                if ((NowTv.sec > (MdTv.sec + 4)) || (NowTv.sec < MdTv.sec))
                    s_mdResults[dev].RetInfo.result = 0;
            }
            ak_thread_mutex_unlock(&s_mdResults[dev].Lock);
            move_count = 0;
        }
    }

    ak_print_normal_ex(MODULE_ID_MD, "### thread id: %ld exit ###\n", ak_thread_get_tid());
    ak_thread_exit();
    return NULL;
}

/* =========================================================
 *  公开接口
 * ========================================================= */
int DrvMotionDetectInit(int dev_id)
{
    if (dev_id >= VIDEO_DEV_MUX) return AK_FAILED;

    if (MdInitFlag[dev_id]) {
        ak_print_error_ex(MODULE_ID_MD, "have been init\n");
        return AK_SUCCESS;
    }

    /* 参数初始化（对应旧版 SvpMdParamInit）*/
    if (s_md_param[dev_id].md_fps == 0)
        apply_default_param(dev_id, &s_md_param[dev_id]);

    MdParam *p = &s_md_param[dev_id];

    memset(&MdCtrl[dev_id],  0, sizeof(struct HwMdCtrl));
    memset(&s_mdResults[dev_id], 0, sizeof(struct HwMdResult));

    MdCtrl[dev_id].MrdHandle = ak_mrd_init();
    if (!MdCtrl[dev_id].MrdHandle) {
        ak_print_error_ex(MODULE_ID_MD, "ak_mrd_init failed!\n");
        return AK_FAILED;
    }

    MdCtrl[dev_id].Dev       = dev_id;
    MdCtrl[dev_id].ThreadStat = MD_THREAD_STOP;
    MdCtrl[dev_id].MdFps     = p->md_fps;
    ak_thread_sem_init(&MdCtrl[dev_id].SendSem, 0);
    ak_thread_mutex_init(&s_mdResults[dev_id].Lock, NULL);

    ak_mrd_set_mode(MdCtrl[dev_id].MrdHandle, 1);
    ak_mrd_set_filters(MdCtrl[dev_id].MrdHandle, p->flt_big, p->flt_small);
    ak_mrd_set_motion_region_size(MdCtrl[dev_id].MrdHandle, p->move_size_min, p->move_size_max);
    ak_mrd_set_floating_wadding_params(MdCtrl[dev_id].MrdHandle, 3, 1, 3, 36, 2, 2);

    if (ak_thread_create(&MdCtrl[dev_id].MdTid, md_thread,
                         &MdCtrl[dev_id].Dev,
                         ANYKA_THREAD_NORMAL_STACK_SIZE, -1) != 0) {
        ak_print_error_ex(MODULE_ID_MD, "create move detect thread failed.\n");
        ak_mrd_destroy(MdCtrl[dev_id].MrdHandle);
        return AK_FAILED;
    }
    ak_thread_detach(MdCtrl[dev_id].MdTid);

    MdInitFlag[dev_id] = 1;
    printf("[DrvMD] dev=%d init ok fps=%d\n", dev_id, p->md_fps);
    return AK_SUCCESS;
}

int DrvMotionDetectEnable(int dev_id, int enable)
{
    if (dev_id >= VIDEO_DEV_MUX) return AK_FAILED;
    if (!MdInitFlag[dev_id]) {
        ak_print_error_ex(MODULE_ID_MD, "fail,no init\n");
        return AK_FAILED;
    }

    ak_print_normal_ex(MODULE_ID_MD, "Enable:%d\n", enable);

    if (enable && MD_THREAD_STOP == MdCtrl[dev_id].ThreadStat)
        MdCtrl[dev_id].ThreadStat = MD_THREAD_RUN;
    else if (!enable && MD_THREAD_RUN == MdCtrl[dev_id].ThreadStat)
        MdCtrl[dev_id].ThreadStat = MD_THREAD_STOP;

    return AK_SUCCESS;
}

int DrvMotionDetectFiltersSet(int dev_id, int flt_big, int flt_small)
{
    if (dev_id >= VIDEO_DEV_MUX) return AK_FAILED;
    if (!MdInitFlag[dev_id]) {
        ak_print_error_ex(MODULE_ID_MD, "fail,no init\n");
        return AK_FAILED;
    }
    ak_mrd_set_filters(MdCtrl[dev_id].MrdHandle, flt_big, flt_small);
    return AK_SUCCESS;
}

/**
 * DrvMotionDetectGetResult - 获取运动检测结果
 *
 * 旧版 MotionDetectGetResult(Dev, RetInfo, Timeout=0) 行为：
 *   - MD 未初始化 或 线程非 RUN 状态 → 返回 AK_FAILED
 *   - MD 运行中，无结果（result==0）  → 返回 AK_SUCCESS（0），out->result=0
 *   - MD 运行中，有结果（result==1）  → 复制结果，清零，返回 AK_SUCCESS
 *
 * 保持此行为是为了让 svc_svp.c 中
 *   if (AK_SUCCESS == DrvMotionDetectGetResult(...)) { ... ak_object_filter_alarm ... }
 * 的逻辑与旧版完全一致。
 */
int DrvMotionDetectGetResult(int dev_id, MdResult *out)
{
    if (dev_id >= VIDEO_DEV_MUX || !out) return AK_FAILED;

    memset(out, 0, sizeof(s_mdResults));

    if (!MdInitFlag[dev_id]) {
        ak_print_error_ex(MODULE_ID_MD, "fail,not init\n");
        return AK_FAILED;
    }

    /* 与旧版完全一致：线程非 RUN 状态时返回 FAILED */
    if (MD_THREAD_RUN != MdCtrl[dev_id].ThreadStat) {
        ak_print_error_ex(MODULE_ID_MD, "fail,not run\n");
        return AK_FAILED;
    }

    /* 有结果：复制并清除（旧版 GetMotionDetectResult 内联逻辑）*/
    ak_thread_mutex_lock(&s_mdResults[dev_id].Lock);
    if (s_mdResults[dev_id].RetInfo.result) {
        *out = s_mdResults[dev_id].RetInfo;
        memset(&s_mdResults[dev_id].RetInfo, 0, sizeof(s_mdResults));
        ak_thread_mutex_unlock(&s_mdResults[dev_id].Lock);
        return AK_SUCCESS;
    }
    ak_thread_mutex_unlock(&s_mdResults[dev_id].Lock);

    /* 无结果但线程在运行：旧版 Timeout=0 分支 "return 0"（== AK_SUCCESS）
     * 调用方通过 out->result==0 判断是否真的有运动。 */
    return AK_SUCCESS;
}

int DrvMotionDetectUninit(int dev_id)
{
    if (dev_id >= VIDEO_DEV_MUX) return AK_FAILED;
    if (!MdInitFlag[dev_id]) {
        ak_print_error_ex(MODULE_ID_MD, "fail,no init\n");
        return AK_FAILED;
    }

    MdInitFlag[dev_id] = 0;
    MdCtrl[dev_id].ThreadStat = MD_THREAD_EXIT;

    /* 旧版使用 ak_thread_join；由于已 detach，改用 sleep 等待退出 */
    ak_thread_sem_post(&MdCtrl[dev_id].SendSem);
    ak_sleep_ms(200);

    ak_mrd_destroy(MdCtrl[dev_id].MrdHandle);
    MdCtrl[dev_id].MrdHandle = NULL;

    ak_thread_sem_destroy(&MdCtrl[dev_id].SendSem);
    ak_thread_mutex_destroy(&s_mdResults[dev_id].Lock);

    printf("[DrvMD] dev=%d uninit ok\n", dev_id);
    return AK_SUCCESS;
}

void DrvMotionDetectSetParam(int dev_id, const MdParam *param)
{
    if (dev_id >= VIDEO_DEV_MUX || !param) return;
    s_md_param[dev_id] = *param;
}
