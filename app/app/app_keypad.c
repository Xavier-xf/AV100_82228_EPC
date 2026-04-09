/**
 * @file    app_keypad.c
 * @brief   数字键盘业务逻辑（菜单状态机）
 *
 * 菜单命令速查（管理员模式内，管理密码默认 999999#）：
 *   099#          → 恢复出厂
 *   08 + 0/1 + #  → 关/开 开锁提示音
 *   077# + 新6位# + 确认#  → 修改管理密码
 *   011# + 新码# + 确认#   → 修改门锁密码（0119#=同时修改出厂默认）
 *   022# + 新码# + 确认#   → 修改门闸密码（0229#=同时修改出厂默认）
 *   2 + 时间(3位)# → 修改开锁时长（29xxx#=修改出厂默认）
 *   4 + 时间(3位)# → 修改开闸时长（49xxx#=修改出厂默认）
 *   1 + 时间(2位)# → 修改键盘背光时长（秒）
 *   3 + 0/1/2 + #  → 设置开锁方式（0=仅卡 1=卡或码 2=卡+码）
 *   5 + 0/1/2 + #  → 设置安防模式
 *   6 + 0/1 + #    → 公开开锁使能
 *   7 + 索引(3位)# → 进入添加卡模式
 *   8 + 888#       → 清空卡组
 *   8 + 999#       → 进入删除卡模式（刷卡删除）
 *   8 + 索引(3位)# → 删除指定卡
 *   9 + 语言(2位)# → 修改语言（99xx#=修改出厂默认）
 *   *#             → 进入修改卡密码待机（再刷卡触发）
 */
#define LOG_TAG "AppKeypad"
#include "log.h"

#include "app_keypad.h"
#include "app_card.h"
#include "app_user_config.h"
#include "drv_keypad.h"
#include "drv_gpio.h"
#include "drv_infrared.h"
#include "svc_timer.h"
#include "svc_voice.h"
#include "svc_net_manage.h"

#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

/* =========================================================
 *  内部类型
 * ========================================================= */
#define ATOI(c)  ((c) - '0')

typedef enum {
    KeyStandby = 0,
    KeyCodeUnlock,
    KeyStandbyReady,
    KeyStandbyReturn,
    KeyReturn,

    KeyNewCardCode,
    KeyAffiCardCode,
    KeyCardCodeVerify,

    KeyAdmin,

    KeyUnlockVoiceSwitch,

    KeyNewAdminCode,
    KeyAffiAdminCode,
    KeyAdminCodeVerify,

    KeyNewLockCode,
    KeyAffiLockCode,
    KeyLockCodeVerify,

    KeyNewGateCode,
    KeyAffiGateCode,
    KeyGateCodeVerify,

    KeyUnlockTime,
    KeyUngateTime,

    KeyReset,

    KeyBacklightTime,

    KeyLockWay,
    KeySafeMode,
    KeyPublicUnlockEn,

    KeyAddCard,
    KeyAddMoreCard,
    KeyDelCard,

    KeyLanguage,
    ActionTotal
} KeyAction;

typedef struct {
    int             Cursor;
    char            Buff[KEYPAD_BUFFER_SIZE];
    struct timespec Time;
} Keyboard;

typedef struct ActionRoute {
    const char *ActionStr;
    const char *Command;                            /* 前缀匹配字符串，NULL=匹配任意 */
    int  (*Process)(Keyboard *, struct ActionRoute *);
    void (*ErrorHandle)(void);
    int  (*ExitHandle)(Keyboard *, struct ActionRoute *);
    KeyAction NextRoute[ActionTotal];
    int       NextRouteCount;
    void     *RouteData;
} ActionRoute;

typedef struct {
    ActionRoute *Routes[ActionTotal];
    int          CurrentRoute;
} RouteStack;

/* =========================================================
 *  状态存储
 * ========================================================= */
static ActionRoute s_routes[ActionTotal];  /* 路由表（运行时含 RouteData）*/
static RouteStack  s_stack;

/* 卡片索引：由 app_card.c 写入 */
static int s_modify_card_idx = -1;   /* 修改卡密码模式的卡索引         */
static int s_add_card_idx    = -1;   /* 从键盘进入添加模式的指定卡索引  */

void AppKeypadSetModifyCardIdx(int idx)
{
    s_modify_card_idx = idx;
}

/* =========================================================
 *  键盘背光
 * ========================================================= */
static void keypad_light_off_cb(void *arg)
{
    (void)arg;
    DrvGpioKeypadLightSet(0);
}

void AppKeypadLightEnable(void)
{
    /* 夜间（红外亮起）：使用配置时长或全天（86400s）；白天：固定 10s */
    int sec = DrvInfraredIsNight()
        ? (AppUserConfigGet()->NumKeyLightTime
               ? AppUserConfigGet()->NumKeyLightTime : 86400)
        : 10;
    SvcTimerSet(TMR_KEYPAD_BACKLIGHT, (unsigned int)(sec * 1000),
                keypad_light_off_cb, NULL);
    DrvGpioKeypadLightSet(1);
}

static void keypad_light_enable(void)
{
    AppKeypadLightEnable();
}

/* =========================================================
 *  路由栈操作
 * ========================================================= */
static void push_route(KeyAction action);
static void pop_route(void);

/* 居中打印*/
static void print_centered(const char *str, int width)
{
    int len = (int)strlen(str);
    int pad = (width - len) / 2;
    if (pad < 0) pad = 0;
    printf("[%*s%s%*s]\n", pad, "", str, width - len - pad, "");
}

static void print_action_stack(void)
{
    print_centered("ActionStack Top", 37);
    for (int i = s_stack.CurrentRoute; i >= 0; i--) {
        if (s_stack.Routes[i])
            print_centered(s_stack.Routes[i]->ActionStr, 30);
    }
    print_centered("ActionStack Bottom", 37);
    printf("\n");
}

static void push_route(KeyAction action)
{
    assert(action >= KeyStandby && action < ActionTotal);
    ActionRoute **cur = &(s_stack.Routes[s_stack.CurrentRoute]);

    if (*cur == NULL) {
        /* 栈空：直接入栈（初始化时专用）*/
        *cur = &s_routes[action];
        LOG_D("Enter %s", (*cur)->ActionStr);
    } else {
        /* 检查 action 是否在当前节点的合法后继中 */
        for (int i = 0; i < (*cur)->NextRouteCount; i++) {
            if ((*cur)->NextRoute[i] == action) {
                s_routes[action].RouteData = (*cur)->RouteData;
                s_stack.CurrentRoute++;
                s_stack.Routes[s_stack.CurrentRoute] = &s_routes[action];

                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                printf("%s => %s [%ld]\n",
                       (*cur)->ActionStr,
                       s_stack.Routes[s_stack.CurrentRoute]->ActionStr,
                       ts.tv_sec);
                break;
            }
        }
    }
}

static void pop_route(void)
{
    if (s_stack.CurrentRoute) {
        ActionRoute *cur = s_stack.Routes[s_stack.CurrentRoute];
        if (cur && cur->ExitHandle)
            cur->ExitHandle(NULL, NULL);
        s_stack.CurrentRoute--;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        printf("%s [%ld]\n",
               s_stack.Routes[s_stack.CurrentRoute]->ActionStr,
               ts.tv_sec);
    }
}

/* =========================================================
 *  超时回调
 * ========================================================= */
static void admin_timeout_cb(void *arg)
{
    (void)arg;
    LOG_I("admin timeout, exit admin mode");
    while (s_stack.CurrentRoute != KeyStandby)
        pop_route();
    SvcVoicePlaySimple(VOICE_Bi3, VOICE_VOL_DEFAULT);
}

static void modify_code_card_timeout_cb(void *arg)
{
    (void)arg;
    printf("ModifyCodeCardTimer[Disable]\n");
    LOG_D("modify code card timeout");
    pop_route();
}

static void add_del_card_timeout_cb(void *arg)
{
    (void)arg;
    LOG_I("add/del card timeout, exit mode");
    pop_route();
}

/* =========================================================
 *  通用错误处理
 * ========================================================= */
static void standby_error(void)
{
    SvcVoicePlaySimple(VOICE_Bi4, VOICE_VOL_DEFAULT);
}

static void error_handle(void)
{
    printf("ErrorHandle\n");
    pop_route();
    pop_route();
    SvcVoicePlaySimple(VOICE_Bi4, VOICE_VOL_DEFAULT);
}

/* =========================================================
 *  各节点 Process 函数
 * ========================================================= */

/* ---- 待机：仅清空 AdminOutTimer ---- */
static int enter_standby(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    SvcTimerStop(TMR_ADMIN_TIMEOUT);
    return 1;
}

/* ---- 密码开锁 ---- */
static int code_unlock(Keyboard *k, ActionRoute *r)
{
    (void)r;
    AppUserConfig *cfg = AppUserConfigGet();
    int perm = 0;   /* 0=无权限，CARD_PERM_LOCK|CARD_PERM_GATE */

    /* 公开密码优先（无论 LockWay 如何）*/
    if (cfg->PublicUnlockEn) {
        int code_len = k->Cursor - 1;
        if (code_len == (int)strlen(cfg->UnlockCode) &&
            memcmp(k->Buff, cfg->UnlockCode, (size_t)code_len) == 0)
            perm |= CARD_PERM_LOCK;
        if (code_len == (int)strlen(cfg->UngateCode) &&
            memcmp(k->Buff, cfg->UngateCode, (size_t)code_len) == 0)
            perm |= CARD_PERM_GATE;
        if (perm)
            goto finish;
    }

    if (cfg->LockWay == UNLOCK_WAY_CARD_ONLY)
        goto error;

    if (cfg->LockWay == UNLOCK_WAY_CARD_AND_CODE) {
        /* 卡+码模式：必须先刷卡（TMR_CODE_CARD_UNLOCK 激活）*/
        if (!SvcTimerActive(TMR_CODE_CARD_UNLOCK))
            goto error;
        int code_len = k->Cursor - 1;
        if (code_len == (int)strlen(CARD_INITIAL_CODE) &&
            memcmp(CARD_INITIAL_CODE, k->Buff, (size_t)code_len) == 0)
            goto error;   /* 初始密码不允许 */
        int card_idx = AppCardCodeCardIdxGet();
        perm = AppCardCodeVerify(card_idx, k->Buff, code_len);
        SvcTimerStop(TMR_CODE_CARD_UNLOCK);
    } else {
        /* 卡或码模式：直接按卡组密码搜索 */
        perm = AppCardCodePermission(k->Buff, k->Cursor - 1);
    }

finish:
    if (perm) {
        AppCardSecurityErrorReset();
        SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
        if (perm & CARD_PERM_LOCK)
            AppCardUnlockAsync(GPIO_LOCK_DOOR, cfg->UnlockTime * 1000, 1);
        if (perm & CARD_PERM_GATE)
            AppCardUnlockAsync(GPIO_LOCK_GATE, cfg->UngateTime * 1000,
                               !(perm & CARD_PERM_LOCK));
        LOG_I("CodeUnlock OK perm=%d", perm);
        return 1;
    }

error:
    AppCardSecurityErrorUpdate();
    LOG_W("CodeUnlock FAIL");
    return 0;
}

/* ---- StandbyReady（按 * 进入）---- */
static int exit_standby_ready(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    if (SvcTimerActive(TMR_MODIFY_CODE_CARD))
        printf("ModifyCodeCardTimer[Disable]\n");
    SvcTimerStop(TMR_MODIFY_CODE_CARD);
    return 1;
}

static int standby_ready_mode(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    if (!SvcTimerActive(TMR_MODIFY_CODE_CARD)) {
        printf("ModifyCodeCardTimer[Enable]\n");
        SvcTimerSet(TMR_MODIFY_CODE_CARD, 30000,
                    modify_code_card_timeout_cb, NULL);
    }
    push_route(KeyStandbyReady);
    return 1;
}

static int return_route(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    pop_route();
    return 1;
}

static int return_standby(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    while (s_stack.CurrentRoute != KeyStandby)
        pop_route();
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

/* ---- 修改卡密码（StandbyReady → 刷卡后进入）---- */
static int enter_modify_card_code(Keyboard *k, ActionRoute *r)
{
    (void)r;
    if (k->Cursor - 1 != CARD_CODE_LEN)
        goto error;
    if (!SvcTimerActive(TMR_MODIFY_CODE_CARD))
        goto error;

    int card_idx = s_modify_card_idx;
    if (card_idx < 0)
        goto error;

    if (AppCardCodeVerify(card_idx, k->Buff, k->Cursor - 1)) {
        SvcTimerStop(TMR_MODIFY_CODE_CARD);
        push_route(KeyNewCardCode);
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        return 1;
    }

error:
    SvcTimerStop(TMR_MODIFY_CODE_CARD);
    return 0;
}

static int enter_affirm_card_code(Keyboard *k, ActionRoute *r)
{
    if (k->Cursor - 1 != CARD_CODE_LEN && r->RouteData)
        return 0;

    static char buf[KEYPAD_BUFFER_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, k->Buff, (size_t)(k->Cursor - 1));
    r->RouteData = buf;
    push_route(KeyAffiCardCode);
    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
    return 1;
}

static int enter_card_code_verify(Keyboard *k, ActionRoute *r)
{
    if (k->Cursor - 1 != CARD_CODE_LEN && r->RouteData)
        goto error;

    int card_idx = s_modify_card_idx;
    if (card_idx < 0)
        goto error;

    if (r->RouteData &&
        memcmp(k->Buff, r->RouteData, (size_t)(k->Cursor - 1)) == 0) {
        LOG_I("modify card[%d] code succeed", card_idx);
        memcpy(AppCardInfoGet()->Deck[card_idx].Code, k->Buff,
               (size_t)(k->Cursor - 1));
        AppCardSave();
        pop_route(); pop_route(); pop_route();
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        return 1;
    }

error:
    pop_route(); pop_route(); pop_route();
    return 0;
}

/* ---- 管理员模式 ---- */
static int exit_admin_mode(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    printf("[ExitAdminMode] Destroy AdminOutTimer[Current Status %s] !!!!!\n",
           SvcTimerActive(TMR_ADMIN_TIMEOUT) ? "true" : "false");
    SvcTimerStop(TMR_ADMIN_TIMEOUT);
    return 1;
}

static int enter_admin_mode(Keyboard *k, ActionRoute *r)
{
    (void)r;
    AppUserConfig *cfg = AppUserConfigGet();
    if (k->Cursor - 1 != (int)strlen(cfg->AdminCode)) {
        printf("[EnterAdminMode]Not enough password bits!!\n");
        return 0;
    }
    if (memcmp(k->Buff, cfg->AdminCode, strlen(cfg->AdminCode)) != 0)
        return 0;

    if (SvcTimerActive(TMR_MODIFY_CODE_CARD))
        printf("ModifyCodeCardTimer[Disable]\n");
    SvcTimerStop(TMR_MODIFY_CODE_CARD);
    push_route(KeyAdmin);
    printf("[EnterAdminMode] Open AdminOutTimer !!!!!\n");
    SvcTimerSet(TMR_ADMIN_TIMEOUT, 30000, admin_timeout_cb, NULL);
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

/* ---- 恢复出厂 ---- */
static int restore_factory(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    AppUserConfigReset();
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
    return 1;
}

/* ---- 开锁提示音开关 ---- */
static int unlock_voice_switch(Keyboard *k, ActionRoute *r)
{
    (void)r;
    int v = ATOI(k->Buff[2]);
    if (v != 0 && v != 1)
        return 0;
    AppUserConfigGet()->UnlockVoiceEn = (char)v;
    AppUserConfigSave();
    AppCardSave();
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

/* ---- 修改管理密码 ---- */
static int enter_new_admin_code(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    push_route(KeyNewAdminCode);
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

static int enter_affirm_admin_code(Keyboard *k, ActionRoute *r)
{
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    if (k->Cursor - 1 != ADMIN_CODE_LEN)
        return 0;

    static char buf[ADMIN_CODE_LEN + 1];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, k->Buff, ADMIN_CODE_LEN);
    r->RouteData = buf;
    push_route(KeyAffiAdminCode);
    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
    return 1;
}

static int enter_admin_code_verify(Keyboard *k, ActionRoute *r)
{
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    if (!r->RouteData || k->Cursor - 1 != ADMIN_CODE_LEN)
        return 0;
    if (memcmp(k->Buff, r->RouteData, ADMIN_CODE_LEN) == 0) {
        LOG_I("modify admin code succeed");
        memcpy(AppUserConfigGet()->AdminCode, k->Buff, ADMIN_CODE_LEN);
        AppUserConfigSave();
        pop_route(); pop_route();
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        return 1;
    }
    LOG_W("admin code verify fail");
    return 0;
}

/* ---- 修改门锁密码 ---- */
static int enter_new_lock_code(Keyboard *k, ActionRoute *r)
{
    (void)r;
    static bool factory_flag;
    factory_flag = false;
    int len = k->Cursor - 1;

    if (len == 4 && ATOI(k->Buff[3]) == 9)
        factory_flag = true;
    else if (len != 3)
        return 0;

    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    push_route(KeyNewLockCode);
    s_routes[KeyNewLockCode].RouteData = &factory_flag;
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

static int enter_affirm_lock_code(Keyboard *k, ActionRoute *r)
{
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int len = k->Cursor - 1;
    if (len > UNLOCK_CODE_MAX_LEN || len < UNLOCK_CODE_MIN_LEN)
        return 0;

    static char buf[UNLOCK_CODE_MAX_LEN + 1];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, k->Buff, (size_t)len);
    push_route(KeyAffiLockCode);
    s_routes[KeyAffiLockCode].RouteData = buf;
    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
    return 1;
}

static int enter_lock_code_verify(Keyboard *k, ActionRoute *r)
{
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int len = k->Cursor - 1;
    if (!r->RouteData || len != (int)strlen((char *)r->RouteData) ||
        len > UNLOCK_CODE_MAX_LEN)
        return 0;

    if (memcmp(k->Buff, r->RouteData, (size_t)len) == 0) {
        bool fac = s_routes[KeyNewLockCode].RouteData
                   ? *(bool *)s_routes[KeyNewLockCode].RouteData : false;
        if (fac) {
            memset(AppUserDefaultConfigGet()->UnlockCode, 0,
                   sizeof(AppUserDefaultConfigGet()->UnlockCode));
            memcpy(AppUserDefaultConfigGet()->UnlockCode, r->RouteData, (size_t)len);
            AppUserDefaultConfigSave();
        }
        memset(AppUserConfigGet()->UnlockCode, 0,
               sizeof(AppUserConfigGet()->UnlockCode));
        memcpy(AppUserConfigGet()->UnlockCode, r->RouteData, (size_t)len);
        AppUserConfigSave();
        pop_route(); pop_route();
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        LOG_I("modify lock code succeed (factory=%d)", fac);
        return 1;
    }
    return 0;
}

/* ---- 修改门闸密码 ---- */
static int enter_new_gate_code(Keyboard *k, ActionRoute *r)
{
    (void)r;
    static bool factory_flag;
    factory_flag = false;
    int len = k->Cursor - 1;

    if (len == 4 && ATOI(k->Buff[3]) == 9)
        factory_flag = true;
    else if (len != 3)
        return 0;

    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    push_route(KeyNewGateCode);
    s_routes[KeyNewGateCode].RouteData = &factory_flag;
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

static int enter_affirm_gate_code(Keyboard *k, ActionRoute *r)
{
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int len = k->Cursor - 1;
    if (len > UNLOCK_CODE_MAX_LEN || len < UNLOCK_CODE_MIN_LEN)
        return 0;

    static char buf[UNLOCK_CODE_MAX_LEN + 1];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, k->Buff, (size_t)len);
    push_route(KeyAffiGateCode);
    s_routes[KeyAffiGateCode].RouteData = buf;
    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
    return 1;
}

static int enter_gate_code_verify(Keyboard *k, ActionRoute *r)
{
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int len = k->Cursor - 1;
    if (!r->RouteData || len != (int)strlen((char *)r->RouteData) ||
        len > UNLOCK_CODE_MAX_LEN)
        return 0;

    if (memcmp(k->Buff, r->RouteData, (size_t)len) == 0) {
        bool fac = s_routes[KeyNewGateCode].RouteData
                   ? *(bool *)s_routes[KeyNewGateCode].RouteData : false;
        if (fac) {
            memset(AppUserDefaultConfigGet()->UngateCode, 0,
                   sizeof(AppUserDefaultConfigGet()->UngateCode));
            memcpy(AppUserDefaultConfigGet()->UngateCode, r->RouteData, (size_t)len);
            AppUserDefaultConfigSave();
        }
        memset(AppUserConfigGet()->UngateCode, 0,
               sizeof(AppUserConfigGet()->UngateCode));
        memcpy(AppUserConfigGet()->UngateCode, r->RouteData, (size_t)len);
        AppUserConfigSave();
        pop_route(); pop_route();
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        LOG_I("modify gate code succeed (factory=%d)", fac);
        return 1;
    }
    return 0;
}

/* ---- 设置开锁时长 ---- */
static int set_unlock_time(Keyboard *k, ActionRoute *r)
{
    (void)r;
    bool fac = false;
    int len = k->Cursor - 1;
    if (len == 5 && ATOI(k->Buff[1]) == 9)
        fac = true;
    else if (len != 4)
        return 0;

    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int base = 1 + (int)fac;
    int t = ATOI(k->Buff[base]) * 100 + ATOI(k->Buff[base+1]) * 10 + ATOI(k->Buff[base+2]);
    if (t == 0) return 0;

    AppUserConfigGet()->UnlockTime = t;
    AppUserConfigSave();
    if (fac) {
        AppUserDefaultConfigGet()->UnlockTime = t;
        AppUserDefaultConfigSave();
    }
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    LOG_I("unlock time = %ds (factory=%d)", t, fac);
    return 1;
}

/* ---- 设置开闸时长 ---- */
static int set_ungate_time(Keyboard *k, ActionRoute *r)
{
    (void)r;
    bool fac = false;
    int len = k->Cursor - 1;
    if (len == 5 && ATOI(k->Buff[1]) == 9)
        fac = true;
    else if (len != 4)
        return 0;

    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int base = 1 + (int)fac;
    int t = ATOI(k->Buff[base]) * 100 + ATOI(k->Buff[base+1]) * 10 + ATOI(k->Buff[base+2]);
    if (t == 0) return 0;

    AppUserConfigGet()->UngateTime = t;
    AppUserConfigSave();
    if (fac) {
        AppUserDefaultConfigGet()->UngateTime = t;
        AppUserDefaultConfigSave();
    }
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    LOG_I("ungate time = %ds (factory=%d)", t, fac);
    return 1;
}

/* ---- 键盘背光时长 ---- */
static int set_backlight_time(Keyboard *k, ActionRoute *r)
{
    (void)r;
    if (k->Cursor != 4) return 0;
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    AppUserConfigGet()->NumKeyLightTime = ATOI(k->Buff[1]) * 10 + ATOI(k->Buff[2]);
    AppUserConfigSave();
    keypad_light_enable();
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    LOG_I("backlight time = %ds", AppUserConfigGet()->NumKeyLightTime);
    return 1;
}

/* ---- 开锁方式 ---- */
static int set_lock_way(Keyboard *k, ActionRoute *r)
{
    (void)r;
    if (k->Cursor != 4) return 0;
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int way = ATOI(k->Buff[1]) * 10 + ATOI(k->Buff[2]);
    if (way != UNLOCK_WAY_CARD_ONLY &&
        way != UNLOCK_WAY_CARD_OR_CODE &&
        way != UNLOCK_WAY_CARD_AND_CODE)
        return 0;
    AppUserConfigGet()->LockWay = (AppUnlockWay)way;
    AppUserConfigSave();
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    LOG_I("LockWay = %d", way);
    return 1;
}

/* ---- 安防模式 ---- */
static int set_safe_mode(Keyboard *k, ActionRoute *r)
{
    (void)r;
    if (k->Cursor != 4) return 0;
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int mode = ATOI(k->Buff[1]) * 10 + ATOI(k->Buff[2]);
    if (mode != APP_SAFE_MODE_OFF &&
        mode != APP_SAFE_MODE_LOCK &&
        mode != APP_SAFE_MODE_ALARM)
        return 0;
    AppUserConfigGet()->SafeMode = (AppSafeMode)mode;
    AppUserConfigSave();
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    LOG_I("SafeMode = %d", mode);
    return 1;
}

/* ---- 公开开锁使能 ---- */
static int set_public_unlock(Keyboard *k, ActionRoute *r)
{
    (void)r;
    if (k->Cursor != 4) return 0;
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    int en = ATOI(k->Buff[1]) * 10 + ATOI(k->Buff[2]);
    if (en != 0 && en != 1) return 0;
    AppUserConfigGet()->PublicUnlockEn = (char)en;
    AppUserConfigSave();
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    LOG_I("PublicUnlockEn = %d", en);
    return 1;
}

/* ---- 添加卡（键盘指定索引）---- */
static int exit_add_card(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    SvcTimerStop(TMR_ADD_CARD);
    return 1;
}

static int add_user_card(Keyboard *k, ActionRoute *r)
{
    (void)r;
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    if (k->Cursor != 5) return 0;

    int idx = ATOI(k->Buff[1]) * 100 + ATOI(k->Buff[2]) * 10 + ATOI(k->Buff[3]);
    if (idx >= DECK_SIZE_MAX)
        return 0;
    if (SvcTimerActive(TMR_ADD_CARD))
        return 0;
    if (AppCardIndexPerm(idx))
        return 0;   /* 该位置已有卡 */

    s_add_card_idx = idx;
    SvcTimerSet(TMR_ADD_CARD, 30000, add_del_card_timeout_cb, NULL);
    push_route(KeyAddCard);
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

static int add_more_user_card(Keyboard *k, ActionRoute *r)
{
    (void)r;
    if (k->Cursor != 4) return 0;
    if (!SvcTimerActive(TMR_ADD_CARD)) return 0;

    int idx = ATOI(k->Buff[0]) * 100 + ATOI(k->Buff[1]) * 10 + ATOI(k->Buff[2]);
    if (idx >= DECK_SIZE_MAX || AppCardIndexPerm(idx))
        return 0;

    s_add_card_idx = idx;
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    SvcTimerRefresh(TMR_ADD_CARD, 30000);
    SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
    return 1;
}

/* ---- 删除卡 ---- */
static int exit_del_card(Keyboard *k, ActionRoute *r)
{
    (void)k; (void)r;
    /* 快速超时（1ms），让定时器自然清理 */
    SvcTimerRefresh(TMR_DEL_CARD, 1);
    return 1;
}

static int del_user_card(Keyboard *k, ActionRoute *r)
{
    (void)r;
    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    if (k->Cursor != 5) return 0;

    int idx = ATOI(k->Buff[1]) * 100 + ATOI(k->Buff[2]) * 10 + ATOI(k->Buff[3]);

    if (idx == 888) {
        AppCardDeckFormat();
        SvcVoicePlaySimple(VOICE_Bi3, VOICE_VOL_DEFAULT);
        LOG_I("deck format done");
        return 1;
    }
    if (idx == 999) {
        if (SvcTimerActive(TMR_DEL_CARD)) return 0;
        SvcTimerSet(TMR_DEL_CARD, 30000, add_del_card_timeout_cb, NULL);
        push_route(KeyDelCard);
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        return 1;
    }
    if (idx < DECK_SIZE_MAX) {
        AppCardSetPerm(idx, 0);
        SvcVoicePlaySimple(VOICE_Bi3, VOICE_VOL_DEFAULT);
        LOG_I("del card[%d] done", idx);
        return 1;
    }
    return 0;
}

/* ---- 修改语言 ---- */
static int set_language(Keyboard *k, ActionRoute *r)
{
    (void)r;
    int ret = 0;
    int cursor = k->Cursor;

    if (cursor == 5 && ATOI(k->Buff[1]) == 9) {
        /* 9 + DEFAULT_FACTORY_SET_FLAG(9) + language(2位) → 修改默认出厂语言 */
        int lang = ATOI(k->Buff[2]) * 10 + ATOI(k->Buff[3]);
        if (lang < APP_LANG_TOTAL) {
            AppUserDefaultConfigGet()->Language = (AppLanguage)lang;
            AppUserConfigGet()->Language        = (AppLanguage)lang;
            AppUserConfigSave();
            AppUserDefaultConfigSave();
            SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
            LOG_I("default language = %d", lang);
            ret = 1;
        }
    } else if (cursor == 4) {
        int lang = ATOI(k->Buff[1]) * 10 + ATOI(k->Buff[2]);
        if (lang < APP_LANG_TOTAL) {
            AppUserConfigGet()->Language = (AppLanguage)lang;
            AppUserConfigSave();
            SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
            LOG_I("language = %d", lang);
            ret = 1;
        }
    }

    SvcTimerRefresh(TMR_ADMIN_TIMEOUT, 30000);
    return ret;
}

/* =========================================================
 *  路由表初始化
 * ========================================================= */
static void routes_init(void)
{
    /* ---- 待机 / 基本操作 ---- */
    s_routes[KeyStandby] = (ActionRoute){
        "[Standby]", NULL, enter_standby, standby_error, NULL,
        {KeyStandbyReady, KeyCodeUnlock}, 2, NULL
    };
    s_routes[KeyCodeUnlock] = (ActionRoute){
        "[CodeUnlock]", NULL, code_unlock, standby_error, NULL,
        {0}, 0, NULL
    };
    s_routes[KeyStandbyReady] = (ActionRoute){
        "[StandbyReady]", "*", standby_ready_mode, error_handle,
        exit_standby_ready,
        {KeyAdmin, KeyNewCardCode}, 2, NULL
    };
    s_routes[KeyStandbyReturn] = (ActionRoute){
        "[ReturnStandby]", "*", return_standby, error_handle, NULL,
        {0}, 0, NULL
    };
    s_routes[KeyReturn] = (ActionRoute){
        "[Return]", "*", return_route, NULL, NULL,
        {0}, 0, NULL
    };

    /* ---- 修改卡密码 ---- */
    s_routes[KeyNewCardCode] = (ActionRoute){
        "[NewCardCode]", NULL, enter_modify_card_code, error_handle, NULL,
        {KeyStandbyReturn, KeyAffiCardCode}, 2, NULL
    };
    s_routes[KeyAffiCardCode] = (ActionRoute){
        "[AffiCardCode]", NULL, enter_affirm_card_code, error_handle, NULL,
        {KeyReturn, KeyCardCodeVerify}, 2, NULL
    };
    s_routes[KeyCardCodeVerify] = (ActionRoute){
        "[CardCodeVerify]", NULL, enter_card_code_verify, error_handle, NULL,
        {0}, 0, NULL
    };

    /* ---- 管理员模式 ---- */
    s_routes[KeyAdmin] = (ActionRoute){
        "[AdminMode]", NULL, enter_admin_mode, error_handle, exit_admin_mode,
        {KeyStandbyReturn, KeyReset, KeyUnlockVoiceSwitch,
         KeyNewAdminCode, KeyNewLockCode, KeyNewGateCode,
         KeyUnlockTime, KeyUngateTime, KeyBacklightTime,
         KeyLockWay, KeySafeMode, KeyPublicUnlockEn,
         KeyAddCard, KeyDelCard, KeyLanguage},
        15, NULL
    };
    s_routes[KeyReset] = (ActionRoute){
        "[Reset]", "099", restore_factory, NULL, NULL,
        {0}, 0, NULL
    };
    s_routes[KeyUnlockVoiceSwitch] = (ActionRoute){
        "[VoiceSwitch]", "08", unlock_voice_switch, error_handle, NULL,
        {0}, 0, NULL
    };

    /* ---- 管理密码 ---- */
    s_routes[KeyNewAdminCode] = (ActionRoute){
        "[NewAdminCode]", "077", enter_new_admin_code, error_handle, NULL,
        {KeyReturn, KeyAffiAdminCode}, 2, NULL
    };
    s_routes[KeyAffiAdminCode] = (ActionRoute){
        "[AffiAdminCode]", NULL, enter_affirm_admin_code, error_handle, NULL,
        {KeyReturn, KeyAdminCodeVerify}, 2, NULL
    };
    s_routes[KeyAdminCodeVerify] = (ActionRoute){
        "[AdminCodeVerify]", NULL, enter_admin_code_verify, error_handle, NULL,
        {0}, 0, NULL
    };

    /* ---- 门锁密码 ---- */
    s_routes[KeyNewLockCode] = (ActionRoute){
        "[NewLockCode]", "011", enter_new_lock_code, error_handle, NULL,
        {KeyReturn, KeyAffiLockCode}, 2, NULL
    };
    s_routes[KeyAffiLockCode] = (ActionRoute){
        "[AffiLockCode]", NULL, enter_affirm_lock_code, error_handle, NULL,
        {KeyReturn, KeyLockCodeVerify}, 2, NULL
    };
    s_routes[KeyLockCodeVerify] = (ActionRoute){
        "[LockCodeVerify]", NULL, enter_lock_code_verify, error_handle, NULL,
        {0}, 0, NULL
    };

    /* ---- 门闸密码 ---- */
    s_routes[KeyNewGateCode] = (ActionRoute){
        "[NewGateCode]", "022", enter_new_gate_code, error_handle, NULL,
        {KeyReturn, KeyAffiGateCode}, 2, NULL
    };
    s_routes[KeyAffiGateCode] = (ActionRoute){
        "[AffiGateCode]", NULL, enter_affirm_gate_code, error_handle, NULL,
        {KeyReturn, KeyGateCodeVerify}, 2, NULL
    };
    s_routes[KeyGateCodeVerify] = (ActionRoute){
        "[GateCodeVerify]", NULL, enter_gate_code_verify, error_handle, NULL,
        {0}, 0, NULL
    };

    /* ---- 参数设置 ---- */
    s_routes[KeyUnlockTime] = (ActionRoute){
        "[UnlockTime]", "2", set_unlock_time, error_handle, NULL,
        {0}, 0, NULL
    };
    s_routes[KeyUngateTime] = (ActionRoute){
        "[UngateTime]", "4", set_ungate_time, error_handle, NULL,
        {0}, 0, NULL
    };
    s_routes[KeyBacklightTime] = (ActionRoute){
        "[BacklightTime]", "1", set_backlight_time, error_handle, NULL,
        {0}, 0, NULL
    };
    s_routes[KeyLockWay] = (ActionRoute){
        "[LockWay]", "3", set_lock_way, error_handle, NULL,
        {0}, 0, NULL
    };
    s_routes[KeySafeMode] = (ActionRoute){
        "[SafeMode]", "5", set_safe_mode, error_handle, NULL,
        {0}, 0, NULL
    };
    s_routes[KeyPublicUnlockEn] = (ActionRoute){
        "[PublicUnlock]", "6", set_public_unlock, error_handle, NULL,
        {0}, 0, NULL
    };

    /* ---- 卡管理 ---- */
    s_routes[KeyAddCard] = (ActionRoute){
        "[AddCard]", "7", add_user_card, error_handle, exit_add_card,
        {KeyReturn, KeyAddMoreCard}, 2, NULL
    };
    s_routes[KeyAddMoreCard] = (ActionRoute){
        "[AddMoreCard]", NULL, add_more_user_card, error_handle, exit_add_card,
        {KeyReturn}, 1, NULL
    };
    s_routes[KeyDelCard] = (ActionRoute){
        "[DelCard]", "8", del_user_card, error_handle, exit_del_card,
        {KeyReturn}, 1, NULL
    };

    /* ---- 语言 ---- */
    s_routes[KeyLanguage] = (ActionRoute){
        "[Language]", "9", set_language, error_handle, NULL,
        {0}, 0, NULL
    };
}

/* =========================================================
 *  按键缓冲处理
 * ========================================================= */
static Keyboard s_kb;

static int kb_process_key(int key)
{
    /* 30s 无操作自动清空缓冲 */
    if (s_kb.Cursor && UtilsDiffMs(&s_kb.Time) > 30000)
        s_kb.Cursor = 0;

    UtilsGetTime(&s_kb.Time);

    if (s_kb.Cursor >= KEYPAD_BUFFER_SIZE) {
        s_kb.Cursor = 0;
        return -1;  /* 溢出 */
    }

    if (key == KEYPAD_KEY_POUND) {
        s_kb.Buff[s_kb.Cursor++] = '#';
        return 1;   /* 命令就绪 */
    }
    if (key == KEYPAD_KEY_STAR) {
        if (s_kb.Cursor) {
            s_kb.Cursor--;  /* 退格 */
            return 0;
        }
        /* 空缓冲区按 * ：立即触发命令（跌落到最后 return 1 的行为）*/
        s_kb.Buff[s_kb.Cursor++] = '*';
        return 1;
    }
    /* 数字键 */
    s_kb.Buff[s_kb.Cursor++] = (char)('0' + key);
    return 0;
}

static void kb_process_command(void)
{
    assert(s_kb.Cursor > 0);
    ActionRoute **cur = &(s_stack.Routes[s_stack.CurrentRoute]);

    for (int i = 0; i < (*cur)->NextRouteCount; i++) {
        ActionRoute *route = &s_routes[(*cur)->NextRoute[i]];
        if (!route->Process) continue;
        if (route->Command == NULL ||
            memcmp(route->Command, s_kb.Buff, strlen(route->Command)) == 0) {
            if (route->Process(&s_kb, *cur)) {
                print_action_stack();
                return;  /* 成功 */
            }
        }
    }

    if ((*cur)->ErrorHandle)
        (*cur)->ErrorHandle();
}

/* =========================================================
 *  按键回调（由 drv_keypad.c 线程调用）
 * ========================================================= */
static void keypad_key_handler(int key)
{
    /* 安防触发保护期内屏蔽 */
    if (SvcTimerActive(TMR_SECURITY_TRIGGER))
        return;

    /* 语音播放期间屏蔽 */
    if (SvcVoiceBusy())
        return;

    /* 网络管理会话期间屏蔽 */
    if (SvcNetManageConnected())
        return;

    int ret = kb_process_key(key);
    keypad_light_enable();

    if (ret < 0) {
        LOG_W("buffer overflow");
        SvcVoicePlaySimple(VOICE_Bi4, VOICE_VOL_DEFAULT);
        return;
    }

    SvcVoicePlaySimple(VOICE_Dio, VOICE_VOL_DEFAULT);

    if (ret > 0) {
        /* # 键按下，处理命令 */
        printf("\r\n");
        kb_process_command();
        s_kb.Cursor = 0;
    } else {
        /* 数字键按下，打印当前输入缓冲*/
        printf("[");
        for (int i = 0; i < KEYPAD_BUFFER_SIZE; i++) {
            if (i < s_kb.Cursor)
                printf("%c", s_kb.Buff[i]);
            else
                printf(" ");
        }
        printf("]\r");
        fflush(stdout);
    }
}

/* =========================================================
 *  初始化
 * ========================================================= */
int AppKeypadInit(void)
{
    memset(&s_stack, 0, sizeof(s_stack));
    memset(&s_kb,    0, sizeof(s_kb));
    routes_init();
    push_route(KeyStandby);
    DrvKeypadSetCallback(keypad_key_handler);
    LOG_I("init ok");
    return 0;
}
