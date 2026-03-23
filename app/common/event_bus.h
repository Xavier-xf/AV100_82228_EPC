/**
 * @file    event_bus.h
 * @brief   事件总线（轻量同步发布订阅）
 *
 * ===================== 设计说明 =====================
 *
 * 【同步执行模型】
 *   EventBusPublish 直接在发布者线程中同步调用所有 handler。
 *   handler 必须执行快速（<1ms），不能有阻塞操作（sleep/mutex等待/IO）。
 *
 * 【线程安全约定】
 *   门口机里所有 EventBusSubscribe 必须在 main() 初始化阶段完成，
 *   在任何工作线程启动之前调用完毕。
 *   这样 Publish 期间只读 s_slots，不存在写竞争，无需加锁。
 *   禁止在工作线程中动态调用 Subscribe/Unsubscribe。
 *
 * 【事件执行顺序】
 *   同一次 Publish：多个 handler 严格按订阅顺序执行。
 *   不同 Publish 调用之间：若来自不同线程，由 OS 调度决定顺序。
 *   门口机实际场景：同一 EventId 极少从多个线程同时发布，无序问题可接受。
 *
 * 【为什么不用队列/异步】
 *   门口机事件数量少（~20个），handler 全部快速，
 *   同步模式代码简单、行为可预测、调试容易，足够用。
 *   若未来某个 handler 需要耗时操作，应在 handler 内启动独立线程，
 *   而不是改为异步事件队列（参考 app_upgrade.c 的 upgrade_thread）。
 */
#ifndef _EVENT_BUS_H_
#define _EVENT_BUS_H_

#include <stdint.h>
#include <stddef.h>

/* =========================================================
 *  EventId 枚举
 *
 *  【重要】从 0 开始连续编号，不使用跳跃值，
 *  确保 s_slots[id] 数组下标不产生内存空洞。
 * ========================================================= */
typedef enum
{
    /* --- ADC / 呼叫按键 --- */
    EVT_CALL_KEY_PRESSED = 0, /* Arg: int* key_index (0-based) */
    EVT_EXIT_BTN_PRESSED,     /* Arg: int* exit_index          */

    /* --- 键盘 --- */
    EVT_KEYPAD_DIGIT,   /* Arg: char digit               */
    EVT_KEYPAD_CONFIRM, /* Arg: char buf[]               */

    /* --- 网络命令（svc_network → app）--- */
    EVT_NET_HEARTBEAT_SEND,     /* Arg: NULL  触发心跳回包       */
    EVT_NET_STREAM_STATUS,      /* Arg: NetStreamStatus*         */
    EVT_NET_CALL_START,         /* Arg: NetCallArg*  室内机接听  */
    EVT_NET_CALL_END,           /* Arg: NULL  室内机挂断         */
    EVT_NET_UNLOCK_CMD,         /* Arg: NetUnlockArg* 远程开锁   */
    EVT_NET_RESET_CMD,          /* Arg: NULL  远程出厂复位       */
    EVT_NET_UPGRADE_CMD,        /* Arg: NetUpgradeArg* 固件升级  */
    EVT_NET_MOTION_SENSITIVITY, /* Arg: int* sensitivity (0-3)   */

    /* --- 对讲流内部 --- */
    EVT_INTERCOM_STREAM_WATCHDOG, /* Arg: NULL  5s 无 StreamStatus  */

    /* --- 系统 --- */
    EVT_SYSTEM_CONFIG_CHANGED,     /* Arg: NULL  配置变更            */
    EVT_SYSTEM_SECURITY_TRIGGERED, /* Arg: NULL  安防触发            */
    EVT_SYSTEM_RESET_CMD,          /* Arg: NULL  出厂复位命令        */

    /* --- 智能视觉 --- */
    EVT_SVP_MOTION_DETECTED, /* Arg: SvpMotionEvent*           */

    /* --- 红外 --- */
    EVT_INFRARED_NIGHT_MODE, /* Arg: NULL  进入夜间模式        */
    EVT_INFRARED_DAY_MODE,   /* Arg: NULL  进入白天模式        */

    /* --- 指纹 --- */
    EVT_FINGERPRINT_VERIFIED, /* Arg: int* perm  验证通过       */
    EVT_FINGERPRINT_ENROLLED, /* Arg: int* slot  录入完成       */
    EVT_FINGERPRINT_ERROR,    /* Arg: NULL  操作失败            */

    /* --- 固件升级（预留扩展点，当前无订阅方）--- */
    /* 说明：
     *   升级失败 → app_upgrade.c 直接调用 SvcNetworkUpgradeReply(FAIL)，无需事件
     *   升级成功 → update.sh 执行后系统重启，代码不会继续运行
     *   若未来需要门口机内部响应升级结果（如恢复灯光/声音），在此处订阅即可 */
    EVT_UPGRADE_START, /* Arg: NULL  升级开始（预留）    */
    EVT_UPGRADE_DONE,  /* Arg: int* ok 升级结果（预留）  */

    /* 哨兵（必须最后）*/
    EVT_ID_MAX
} EventId;

/* EVT_ID_MAX 当前值（便于静态断言）*/
/* 约 24 个事件，s_slots 数组占 24 × (8指针+1计数) × 8字节 ≈ 1.5KB，完全可接受 */

/* =========================================================
 *  跨层数据结构（EventBus 传递的 arg 类型）
 * ========================================================= */

/** 远程开锁参数（UnlockEvent 0x54）*/
typedef struct
{
    uint8_t unlock_time;  /* 开锁时长（秒）                     */
    uint8_t lock_type;    /* 0=LOCK 1=GATE 3=LOCK+GATE         */
    uint8_t language_idx; /* 语言索引（对应 AppLanguage 枚举）  */
} NetUnlockArg;

/** 室内机接听参数（OutdoorTalkEvent 0x57）*/
typedef struct
{
    uint8_t sender_dev; /* 室内机设备 ID                      */
    uint8_t channel;    /* CommCh（1=DOOR1 2=DOOR2）          */
} NetCallArg;

/** 流控/监控保活参数（StreamStatusEvent 0x59）*/
typedef struct
{
    uint8_t sender_dev;       /* 发送方设备 ID                   */
    uint8_t key_frame_req;    /* 1=需要关键帧                    */
    uint8_t leave_msg_enable; /* 1=留言模式                      */
    uint8_t leave_msg_lang;   /* 留言语言索引（Arg[0]>>2）       */
    uint8_t monitor_enable;   /* 1=涂鸦 APP 监控模式             */
    uint8_t audio_volume;     /* 音量（Arg[1]*3+66）             */
} NetStreamStatus;

/** 固件升级参数（UpgraedOutdoorEvent 0x62）*/
typedef struct
{
    uint8_t sender_dev; /* 室内机设备 ID                      */
    int is_long_pack;   /* 1=数据包 0=控制包                  */
    uint8_t ctrl_arg0;  /* 控制包 Arg[0]                      */
    uint8_t ctrl_arg1;  /* 控制包 Arg[1]                      */
    uint32_t arg1;      /* 长包：包序号                       */
    uint32_t arg2;      /* 长包：数据长度                     */
    uint8_t *data;      /* 长包：数据指针（指向接收缓冲）     */
    int data_len;       /* 长包：实际数据长度                 */
} NetUpgradeArg;

/** SVP 移动侦测结果 */
typedef struct
{
    int total;       /* 检测到的目标数量                   */
    int boxes_valid; /* 1=boxes有效                        */
    struct
    {
        int x, y, w, h; /* 检测框（主视频通道坐标系）         */
        int score;      /* 置信度                             */
    } boxes[4];
} SvpMotionEvent;

/* =========================================================
 *  EventBus 接口
 * ========================================================= */
typedef void (*EventHandler)(EventId id, const void *arg, size_t len);

/**
 * @brief 初始化事件总线（清零所有 slot）
 * 必须在任何 Subscribe/Publish 之前调用。
 */
void EventBusInit(void);

/**
 * @brief 订阅事件
 *
 * ⚠️ 只能在 main() 初始化阶段调用（工作线程启动前）。
 * 禁止在工作线程运行时动态调用。
 *
 * @return 0=成功 -1=参数错误或 slot 已满
 */
int EventBusSubscribe(EventId id, EventHandler handler);

/**
 * @brief 取消订阅（谨慎使用，同样只在初始化阶段）
 */
int EventBusUnsubscribe(EventId id, EventHandler handler);

/**
 * @brief 发布事件（同步执行所有 handler）
 *
 * 在发布者线程中直接调用 handler，handler 返回后 Publish 才返回。
 * 可在任意线程调用（Publish 期间只读 s_slots）。
 *
 * @param id   事件 ID
 * @param arg  事件数据指针（handler 执行完后可能失效，handler 内部需及时读取）
 * @param len  数据长度（用于校验，handler 可忽略）
 */
void EventBusPublish(EventId id, const void *arg, size_t len);

#endif /* _EVENT_BUS_H_ */
