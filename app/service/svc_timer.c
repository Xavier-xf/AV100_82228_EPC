/**
 * @file    svc_timer.c
 * @brief   软件定时器服务
 *
 * 实现原理：
 *   专用线程循环检查定时器到期，到期后解锁调用回调。
 *
 *   休眠策略：每次循环计算最近截止时间，用 clock_nanosleep(ABSTIME)
 *   精确休眠到该时间点，既提高精度又大幅减少 CPU 唤醒次数。
 *   无活跃定时器时最多休眠 TICK_MAX_MS（200ms），确保新加定时器能及时检测到。
 *
 *   线程安全：s_tmr.mutex 保护 slots 读写。
 *   回调在锁外调用（先取 cb+arg，解锁，再调 cb），回调内可安全调 SvcTimerSet/Stop。
 */
#define LOG_TAG "SvcTimer"
#include "log.h"

#include "svc_timer.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* 无活跃定时器时的最大休眠时间（毫秒）*/
#define TICK_MAX_MS  200

typedef struct {
    SvcTimerCallback cb;
    void            *arg;
    unsigned int     timeout_ms;
    struct timespec  deadline;
    int              active;
} TimerSlot;

typedef struct {
    pthread_mutex_t mutex;
    TimerSlot       slots[TMR_ID_MAX];
} SvcTimerCtx;

static SvcTimerCtx s_tmr = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* =========================================================
 *  时间工具
 * ========================================================= */
static void clock_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static long ts_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (long)(a->tv_sec  - b->tv_sec)  * 1000L
         + (long)(a->tv_nsec - b->tv_nsec) / 1000000L;
}

static void set_deadline(struct timespec *dl, unsigned int ms)
{
    clock_now(dl);
    dl->tv_sec  += (time_t)(ms / 1000);
    dl->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (dl->tv_nsec >= 1000000000L) {
        dl->tv_sec++;
        dl->tv_nsec -= 1000000000L;
    }
}

/* =========================================================
 *  定时器线程（动态休眠到最近截止时间）
 * ========================================================= */
static void *timer_thread(void *arg)
{
    (void)arg;

    while (1) {
        struct timespec now;
        clock_now(&now);

        struct timespec next_wake;
        set_deadline(&next_wake, TICK_MAX_MS);

        for (int i = 0; i < TMR_ID_MAX; i++) {
            pthread_mutex_lock(&s_tmr.mutex);

            if (!s_tmr.slots[i].active) {
                pthread_mutex_unlock(&s_tmr.mutex);
                continue;
            }

            long diff = ts_diff_ms(&now, &s_tmr.slots[i].deadline);

            if (diff < 0) {
                /* 未到期：更新 next_wake 为更早的截止时间 */
                if (ts_diff_ms(&s_tmr.slots[i].deadline, &next_wake) < 0)
                    next_wake = s_tmr.slots[i].deadline;
                pthread_mutex_unlock(&s_tmr.mutex);
                continue;
            }

            /* 已到期：取出 cb/arg，解锁后调用 */
            s_tmr.slots[i].active = 0;
            SvcTimerCallback cb     = s_tmr.slots[i].cb;
            void            *cb_arg = s_tmr.slots[i].arg;
            pthread_mutex_unlock(&s_tmr.mutex);

            if (cb) cb(cb_arg);
        }

        /* 精确休眠到 next_wake，EINTR 时重新循环 */
        int ret;
        do {
            ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL);
        } while (ret == EINTR);
    }
    return NULL;
}

/* =========================================================
 *  公开接口
 * ========================================================= */
int SvcTimerInit(void)
{
    memset(s_tmr.slots, 0, sizeof(s_tmr.slots));

    pthread_t tid;
    if (pthread_create(&tid, NULL, timer_thread, NULL) != 0) {
        LOG_E("create thread fail");
        return -1;
    }
    pthread_detach(tid);
    LOG_I("init ok (TICK_MAX=%dms)", TICK_MAX_MS);
    return 0;
}

int SvcTimerSet(TimerId id, unsigned int timeout_ms,
                SvcTimerCallback cb, void *arg)
{
    if (id >= TMR_ID_MAX || id < 0) return -1;

    pthread_mutex_lock(&s_tmr.mutex);
    s_tmr.slots[id].cb         = cb;
    s_tmr.slots[id].arg        = arg;
    s_tmr.slots[id].timeout_ms = timeout_ms;
    set_deadline(&s_tmr.slots[id].deadline, timeout_ms);
    s_tmr.slots[id].active     = 1;
    pthread_mutex_unlock(&s_tmr.mutex);
    return 0;
}

int SvcTimerRefresh(TimerId id, unsigned int timeout_ms)
{
    if (id >= TMR_ID_MAX || id < 0) return -1;

    pthread_mutex_lock(&s_tmr.mutex);
    if (!s_tmr.slots[id].active && s_tmr.slots[id].cb == NULL) {
        pthread_mutex_unlock(&s_tmr.mutex);
        return -1;
    }
    s_tmr.slots[id].timeout_ms = timeout_ms;
    set_deadline(&s_tmr.slots[id].deadline, timeout_ms);
    s_tmr.slots[id].active     = 1;
    pthread_mutex_unlock(&s_tmr.mutex);
    return 0;
}

void SvcTimerStop(TimerId id)
{
    if (id >= TMR_ID_MAX || id < 0) return;
    pthread_mutex_lock(&s_tmr.mutex);
    s_tmr.slots[id].active = 0;
    pthread_mutex_unlock(&s_tmr.mutex);
}

int SvcTimerActive(TimerId id)
{
    if (id >= TMR_ID_MAX || id < 0) return 0;
    pthread_mutex_lock(&s_tmr.mutex);
    int v = s_tmr.slots[id].active;
    pthread_mutex_unlock(&s_tmr.mutex);
    return v;
}
