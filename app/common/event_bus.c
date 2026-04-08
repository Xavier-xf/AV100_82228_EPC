/**
 * @file    event_bus.c
 * @brief   事件总线实现
 *
 * ===================== 线程安全模型 =====================
 *
 * 【约定】Subscribe 只在 main() Init 阶段（单线程）调用，
 *         Publish 可在任意线程调用（多线程只读，无竞争）。
 *
 * 【为什么不加锁】
 *   Publish 期间只读 s_slots（count + handlers 指针），
 *   Subscribe 只在 Init 阶段单线程写入，两者时间上不重叠。
 *   加 mutex 的额外开销（约1-5μs/次）对高频事件（如 KEYPAD_DIGIT）
 *   反而是负担，且在当前约束下不必要。
 *
 *   如果未来需要运行时动态订阅，需要改成读写锁（pthread_rwlock）：
 *     Subscribe/Unsubscribe → 写锁
 *     Publish → 读锁
 *
 * ===================== 性能数据 =====================
 *
 *   EVT_ID_MAX = 24
 *   MAX_HANDLERS_PER_EVENT = 8
 *   s_slots 总大小 = 24 × (8×8 + 4) = 24 × 68 = 1632 字节（约 1.6KB）
 *   Publish 调用开销（0个handler）= ~10ns
 *   Publish 调用开销（1个handler）= handler 本身耗时
 */
#define LOG_TAG "EventBus"
#include "log.h"

#include "event_bus.h"
#include <string.h>



#define MAX_HANDLERS_PER_EVENT 8

/* 静态断言：确保 EVT_ID_MAX 是合理值（防止枚举意外变大）*/
typedef char assert_evt_max_reasonable[(EVT_ID_MAX <= 64) ? 1 : -1];

typedef struct
{
    EventHandler handlers[MAX_HANDLERS_PER_EVENT];
    int count;
} EventSlot;

static EventSlot s_slots[EVT_ID_MAX];

/* =========================================================
 *  调试用：事件名称表（仅 Debug 模式使用）
 * ========================================================= */
#ifdef EVENT_BUS_DEBUG
static const char *s_evt_names[EVT_ID_MAX] = {
    [EVT_CALL_KEY_PRESSED] = "CALL_KEY_PRESSED",
    [EVT_EXIT_BTN_PRESSED] = "EXIT_BTN_PRESSED",
    [EVT_KEYPAD_DIGIT] = "KEYPAD_DIGIT",
    [EVT_KEYPAD_CONFIRM] = "KEYPAD_CONFIRM",
    [EVT_NET_HEARTBEAT_SEND] = "NET_HEARTBEAT",
    [EVT_NET_STREAM_STATUS] = "NET_STREAM_STATUS",
    [EVT_NET_CALL_START] = "NET_CALL_START",
    [EVT_NET_CALL_END] = "NET_CALL_END",
    [EVT_NET_UNLOCK_CMD] = "NET_UNLOCK_CMD",
    [EVT_NET_RESET_CMD] = "NET_RESET_CMD",
    [EVT_NET_UPGRADE_CMD] = "NET_UPGRADE_CMD",
    [EVT_NET_MOTION_SENSITIVITY] = "NET_MOTION_SENS",
    [EVT_INTERCOM_STREAM_WATCHDOG] = "STREAM_WATCHDOG",
    [EVT_SYSTEM_CONFIG_CHANGED] = "CONFIG_CHANGED",
    [EVT_SYSTEM_SECURITY_TRIGGERED] = "SECURITY_TRIGGERED",
    [EVT_SYSTEM_RESET_CMD] = "SYSTEM_RESET",
    [EVT_SVP_MOTION_DETECTED] = "SVP_MOTION",
    [EVT_INFRARED_NIGHT_MODE] = "INFRARED_NIGHT",
    [EVT_INFRARED_DAY_MODE] = "INFRARED_DAY",
    [EVT_FINGERPRINT_VERIFIED] = "FP_VERIFIED",
    [EVT_FINGERPRINT_ENROLLED] = "FP_ENROLLED",
    [EVT_FINGERPRINT_ERROR] = "FP_ERROR",
    [EVT_UPGRADE_START] = "UPGRADE_START",
    [EVT_UPGRADE_DONE] = "UPGRADE_DONE",
};
#define EVT_NAME(id) \
    (((id) < EVT_ID_MAX && s_evt_names[id]) ? s_evt_names[id] : "?")
#endif

/* =========================================================
 *  接口实现
 * ========================================================= */
void EventBusInit(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    LOG_I("init ok, %d event slots, %d handlers/slot",
          EVT_ID_MAX, MAX_HANDLERS_PER_EVENT);
}

int EventBusSubscribe(EventId id, EventHandler handler)
{
    if (id >= EVT_ID_MAX || handler == NULL)
    {
        LOG_E("Subscribe: invalid id=%d or NULL handler", id);
        return -1;
    }
    EventSlot *slot = &s_slots[id];
    if (slot->count >= MAX_HANDLERS_PER_EVENT)
    {
        LOG_E("Subscribe: slot full for id=%d (max=%d)", id, MAX_HANDLERS_PER_EVENT);
        return -1;
    }
    /* 防止重复订阅 */
    for (int i = 0; i < slot->count; i++)
    {
        if (slot->handlers[i] == handler)
        {
            LOG_W("Subscribe: duplicate handler for id=%d", id);
            return 0; /* 幂等，不报错 */
        }
    }
    slot->handlers[slot->count++] = handler;

#ifdef EVENT_BUS_DEBUG
    LOG_D("Subscribe: id=%d(%s) handler#%d", id, EVT_NAME(id), slot->count);
#endif
    return 0;
}

int EventBusUnsubscribe(EventId id, EventHandler handler)
{
    if (id >= EVT_ID_MAX)
        return -1;
    EventSlot *slot = &s_slots[id];
    for (int i = 0; i < slot->count; i++)
    {
        if (slot->handlers[i] == handler)
        {
            /* 用最后一个填补空位（顺序可能改变，但门口机不依赖 Unsubscribe 顺序）*/
            slot->handlers[i] = slot->handlers[--slot->count];
            slot->handlers[slot->count] = NULL;
            return 0;
        }
    }
    return -1;
}

void EventBusPublish(EventId id, const void *arg, size_t len)
{
    if (id >= EVT_ID_MAX)
        return;

    const EventSlot *slot = &s_slots[id];
    int count = slot->count; /* 读一次 count，避免迭代中被（假设的）并发修改影响 */

#ifdef EVENT_BUS_DEBUG
    if (count > 0)
        LOG_D("Publish: id=%d(%s) → %d handler(s)", id, EVT_NAME(id), count);
#endif

    for (int i = 0; i < count; i++)
    {
        EventHandler h = slot->handlers[i]; /* 读到局部变量，更安全 */
        if (h)
            h(id, arg, len);
    }
}
