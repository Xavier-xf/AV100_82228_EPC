/**
 * @file    svc_timer.h
 * @brief   软件定时器服务（原 Timer.c 重写）
 *
 * 设计要点：
 *   所有定时器 ID 在此集中枚举，全局唯一。
 *   回调在定时器线程中执行，回调内可安全调用 SvcTimerSet/Stop/Refresh。
 *   线程安全：所有接口均可在任意线程调用。
 *
 * 原版定时器 ID 对照（原 Timer.h TimerID 枚举）：
 *   CallBusyTimer       → TMR_CALL_BUSY
 *   AmpTimer            → TMR_AMP_OFF
 *   KeypadTimer         → TMR_KEYPAD_BACKLIGHT
 *   KeypadInputTimer    → TMR_KEYPAD_INPUT_TIMEOUT
 *   CommunicateTimer    → TMR_INTERCOM_WATCHDOG
 *   MonitorTimer        → (已合并到 WATCHDOG)
 *   SVPTimer            → TMR_SVP_ACTIVE
 *   IrFeedTimer         → TMR_IR_DEBOUNCE
 *   IrCurCloseTimer     → TMR_IRCUT_CLOSE
 *   LockTimer           → TMR_UNLOCK_HOLD
 *   GateTimer           → TMR_UNGATE_HOLD
 */
#ifndef _SVC_TIMER_H_
#define _SVC_TIMER_H_

#include <stddef.h>

/* =========================================================
 *  定时器 ID 枚举
 *  ★ 新增定时器在此追加，禁止在各模块散落定义 ★
 * ========================================================= */
typedef enum {
    /* --- 音频/功放 --- */
    TMR_CALL_BUSY        = 0,  /* 门铃呼叫忙音延迟（400ms/3500ms）      */
    TMR_AMP_OFF,               /* 功放延迟关闭（最后一帧播完后 3s）     */

    /* --- 键盘 --- */
    TMR_KEYPAD_BACKLIGHT,      /* 键盘背光自动关闭                      */
    TMR_KEYPAD_INPUT_TIMEOUT,  /* 键盘输入超时（30s 清空缓冲）          */

    /* --- 对讲/监控 --- */
    TMR_INTERCOM_WATCHDOG,     /* 对讲心跳看门狗（5s 无 StreamStatus）  */

    /* --- 安防 --- */
    TMR_SECURITY_TRIGGER,      /* 安防触发保护时间                      */
    TMR_SECURITY_ERROR,        /* 安防错误次数统计窗口（超时清零）      */
    TMR_ALARM_LIGHT,           /* 报警灯闪烁（500ms 循环）              */

    /* --- 继电器 --- */
    TMR_UNLOCK_HOLD,           /* 门锁保持时间（对应原版 LockTimer）    */
    TMR_UNGATE_HOLD,           /* 门闸保持时间（对应原版 GateTimer）    */

    /* --- 键盘管理菜单 --- */
    TMR_ADMIN_TIMEOUT,         /* 管理员模式操作超时（30s）             */
    TMR_MODIFY_CODE_CARD,      /* 修改卡密码操作超时（30s）             */
    TMR_ADD_CARD,              /* 添加 IC 卡等待超时（30s）             */
    TMR_DEL_CARD,              /* 删除 IC 卡等待超时（30s）             */
    TMR_CODE_CARD_UNLOCK,      /* 卡+码组合开锁：等待输入密码           */

    /* --- 灯光 --- */
    TMR_CARD_LIGHT,            /* 刷卡指示灯点亮（200ms 亮 / 1800ms 灭）*/
    TMR_CARD_LIGHT_FLASH,      /* 添加模式刷卡灯快闪（200ms 循环）      */
    TMR_CALL_LIGHT_HIGH,       /* 通话中再按键→高亮 100ms/2000ms        */
    TMR_INFRARED_ACTIVE,       /* 移动侦测触发红外灯保活（30s）          */

    /* --- 视觉 --- */
    TMR_SVP_ACTIVE,            /* SVP 人形检测激活保活（3s）            */

    /* --- 红外夜视 ---
     *   TMR_IR_DEBOUNCE  对应原版 IrFeedTimer（光敏变化 1s 去抖）
     *   TMR_IRCUT_CLOSE  对应原版 IrCurCloseTimer（IRCUT 电机 100ms 停止）
     */
    TMR_IR_DEBOUNCE,           /* 光敏变化去抖（1s）                    */
    TMR_IRCUT_CLOSE,           /* IRCUT 电机停止延迟（100ms）           */

    /* --- 指纹 --- */
    TMR_FINGER_TIMEOUT,        /* 指纹采集超时（30s）                   */

    /* --- 固件升级 --- */
    TMR_UPGRADE_WATCHDOG,      /* 升级超时保护（5min）                  */

    /* --- 网络管理 --- */
    TMR_MANAGE_LIGHT,          /* 添加卡模式指示灯闪烁（200ms 循环）    */

    TMR_ID_MAX                 /* 定时器槽位总数，勿手动赋值            */
} TimerId;

/* 当前 TMR_ID_MAX = 25，svc_timer.c 中断言不超过 64 */

typedef void (*SvcTimerCallback)(void *arg);

/**
 * @brief 启动/重启一个定时器
 * @param id         定时器 ID
 * @param timeout_ms 超时时间（毫秒）
 * @param cb         超时回调（NULL 则仅计时不回调）
 * @param arg        回调参数
 * @return 0 成功，-1 失败
 */
int SvcTimerSet(TimerId id, unsigned int timeout_ms,
                SvcTimerCallback cb, void *arg);

/**
 * @brief 刷新已激活定时器的超时时间（重新计时）
 * @return 0 成功，-1 定时器未曾设置
 */
int SvcTimerRefresh(TimerId id, unsigned int timeout_ms);

/**
 * @brief 停止定时器
 */
void SvcTimerStop(TimerId id);

/**
 * @brief 查询定时器是否激活（已启动且未超时）
 * @return 1 激活，0 未激活
 */
int SvcTimerActive(TimerId id);

/**
 * @brief 初始化定时器服务（在 main() 中调用）
 */
int SvcTimerInit(void);

#endif /* _SVC_TIMER_H_ */
