/**
 * @file    app_card.c
 * @brief   IC 卡管理（数据库 + 刷卡业务逻辑）
 *
 * 硬件读卡由 drv_card.c 完成，刷卡结果通过回调传入 AppCardHandle。
 */
#define LOG_TAG "AppCard"
#include "log.h"

#include "app_card.h"
#include "app_keypad.h"
#include "app_user_config.h"
#include "event_bus.h"
#include "drv_card.h"
#include "drv_gpio.h"
#include "svc_timer.h"
#include "svc_voice.h"
#include "svc_net_manage.h"
#include "utils.h"
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>

/* =========================================================
 *  内部数据
 * ========================================================= */
static AppCardInfo s_deck = {
    .DeckSize = 0,
    .Deck[0 ... (DECK_SIZE_MAX - 1)].Perm = 0
};

/* 卡+码组合模式：刷卡后存储待验证的卡索引，供 app_keypad.c 读取 */
static int s_code_card_idx = -1;

int AppCardCodeCardIdxGet(void) { return s_code_card_idx; }

/* 前向声明 */
static void unlock_async(GpioLockType type, int duration_ms, int play_voice);
static void security_error_update(void);

/* =========================================================
 *  卡组数据库
 * ========================================================= */

int AppCardSave(void)
{
    int fd = open(CARD_CONFIG_PATH, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        LOG_E("open %s fail", CARD_CONFIG_PATH);
        return 0;
    }
    write(fd, &s_deck, sizeof(AppCardInfo));
    close(fd);
    system("fsync -d " CARD_CONFIG_PATH);
    return 1;
}

AppCardInfo *AppCardInfoGet(void)
{
    return &s_deck;
}

static void deck_check(void)
{
    s_deck.DeckSize = 0;
    for (int i = 0; i < DECK_SIZE_MAX; i++) {
        char *d = s_deck.Deck[i].Data;
        if ((d[0] ^ d[1] ^ d[2] ^ d[3]) != d[4]) {
            s_deck.Deck[i].Perm = 0;
        } else if (s_deck.Deck[i].Perm) {
            s_deck.DeckSize++;
        }
    }
    AppCardSave();
}

int AppCardDeckInit(void)
{
    int fd = open(CARD_CONFIG_PATH, O_RDONLY);
    if (fd < 0) {
        LOG_W("%s not found, creating empty deck", CARD_CONFIG_PATH);
        AppCardSave();
        return 0;
    }
    read(fd, &s_deck, sizeof(AppCardInfo));
    close(fd);
    deck_check();
    return 1;
}

int AppCardDeckFormat(void)
{
    s_deck.DeckSize = 0;
    memset(s_deck.Deck, 0, sizeof(s_deck.Deck));
    AppCardSave();
    return 1;
}

int AppCardAdd(int index, char *data, char permissions)
{
    if (index >= DECK_SIZE_MAX || s_deck.DeckSize >= DECK_SIZE_MAX)
        return 0;

    if (index == -1) {
        for (index = 0; index < DECK_SIZE_MAX; index++) {
            if (s_deck.Deck[index].Perm == 0) {
                memcpy(s_deck.Deck[index].Data, data, sizeof(s_deck.Deck[index].Data));
                memcpy(s_deck.Deck[index].Code, CARD_INITIAL_CODE, sizeof(s_deck.Deck[index].Code));
                s_deck.DeckSize++;
                break;
            }
        }
    } else if (s_deck.Deck[index].Perm != 0) {
        char *d = s_deck.Deck[index].Data;
        if ((d[0] ^ d[1] ^ d[2] ^ d[3]) != d[4]) {
            LOG_W("card[%d] data error, replacing", index);
            deck_check();
            memcpy(s_deck.Deck[index].Data, data, sizeof(s_deck.Deck[index].Data));
            memcpy(s_deck.Deck[index].Code, CARD_INITIAL_CODE, sizeof(s_deck.Deck[index].Code));
            s_deck.DeckSize++;
        } else if (memcmp(s_deck.Deck[index].Data, data, sizeof(s_deck.Deck[index].Data)) != 0) {
            return 0;
        }
    } else {
        memcpy(s_deck.Deck[index].Data, data, sizeof(s_deck.Deck[index].Data));
        memcpy(s_deck.Deck[index].Code, CARD_INITIAL_CODE, sizeof(s_deck.Deck[index].Code));
        s_deck.DeckSize++;
    }
    s_deck.Deck[index].Perm |= permissions;
    AppCardSave();
    return 1;
}

int AppCardSetPerm(int index, char permissions)
{
    if (index >= DECK_SIZE_MAX)
        return 0;
    if (permissions < 0 || permissions > 3)
        return 0;
    if (s_deck.Deck[index].Perm == 0)
        return 0;

    if (!(s_deck.Deck[index].Perm = permissions)) {
        memset(s_deck.Deck[index].Data, 0, sizeof(s_deck.Deck[index].Data));
        if (s_deck.DeckSize) s_deck.DeckSize--;
    }
    AppCardSave();
    return 1;
}

int AppCardSearch(char *data)
{
    if (!s_deck.DeckSize)
        return -1;

    int remaining = s_deck.DeckSize;
    for (int i = 0; i < DECK_SIZE_MAX; i++) {
        if (s_deck.Deck[i].Perm) {
            if (memcmp(s_deck.Deck[i].Data, data, sizeof(s_deck.Deck[i].Data)) == 0)
                return i;
            if (!(--remaining))
                break;
        }
    }
    return -1;
}

int AppCardCodeVerify(int index, char *code, int code_len)
{
    if (code_len != sizeof(s_deck.Deck[0].Code))
        return 0;
    if (memcmp(s_deck.Deck[index].Code, code, code_len) == 0)
        return s_deck.Deck[index].Perm;
    return 0;
}

int AppCardCodePermission(char *code, int code_len)
{
    if (code_len != sizeof(s_deck.Deck[0].Code))
        return 0;
    if (memcmp(code, CARD_INITIAL_CODE, strlen(CARD_INITIAL_CODE)) == 0)
        return 0;

    int remaining = s_deck.DeckSize;
    for (int i = 0; i < DECK_SIZE_MAX; i++) {
        if (s_deck.Deck[i].Perm) {
            if (memcmp(s_deck.Deck[i].Code, code, sizeof(s_deck.Deck[i].Code)) == 0)
                return s_deck.Deck[i].Perm;
            if (!(--remaining))
                break;
        }
    }
    return 0;
}

char AppCardIndexPerm(int index)
{
    if (index >= DECK_SIZE_MAX) return 0;
    return s_deck.Deck[index].Perm;
}

int AppCardDeckPermGet(unsigned char **deck)
{
    /* 固定 1280 字节格式：
     * [0]       = DeckSize
     * [1..1200] = 200 * 6 字节（Perm(1) + Data(5)）
     * [1201..1279] = 零填充
     */
    static unsigned char buf[1280];
    memset(buf, 0, sizeof(buf));
    buf[0] = (unsigned char)s_deck.DeckSize;
    for (int i = 0; i < DECK_SIZE_MAX; i++) {
        int off = 1 + i * 6;
        buf[off] = (unsigned char)s_deck.Deck[i].Perm;
        memcpy(&buf[off + 1], s_deck.Deck[i].Data, CARD_DATA_LEN);
    }
    *deck = buf;
    return (int)sizeof(buf);  /* 1280 */
}

/* =========================================================
 *  开锁（异步，避免阻塞刷卡线程）
 *
 *  play_voice=1 时在开锁前播放语言提示音。
 *  调用方确保同一次开锁动作只有第一路锁传 play_voice=1，避免重复播音。
 *  开锁期间重复刷卡不会再次触发开锁
 *  和语音播报，需等开锁时间结束后才能再次刷卡开锁。
 * ========================================================= */
typedef struct {
    GpioLockType type;
    int          duration_ms;
    int          play_voice;  /* 1=播放开锁提示音 */
} UnlockArg;

/* 开锁忙标志：开锁期间置 1，开锁结束后清 0 */
static volatile int s_lock_busy[2] = {0, 0}; /* [0]=DOOR, [1]=GATE */

static inline int lock_busy_idx(GpioLockType type)
{
    return (type == GPIO_LOCK_GATE) ? 1 : 0;
}

static void *unlock_thread(void *arg)
{
    UnlockArg *ua = (UnlockArg *)arg;

    if (ua->play_voice) {
        AppUserConfig *cfg = AppUserConfigGet();
        if (cfg->UnlockVoiceEn) {
            /* Language 枚举值与 VOICE_UnlockEng 偏移量一一对应 */
            int voice_idx = (int)VOICE_UnlockEng + (int)cfg->Language;
            if (voice_idx >= VOICE_UnlockEng && voice_idx < VOICE_TOTAL)
                SvcVoicePlaySimple((VoiceId)voice_idx, VOICE_VOL_DEFAULT);
        }
    }

    DrvGpioLockOpen(ua->type, ua->duration_ms);
    s_lock_busy[lock_busy_idx(ua->type)] = 0;
    free(ua);
    return NULL;
}

/* 公开给 app_keypad.c 调用 */
void AppCardUnlockAsync(int type, int duration_ms, int play_voice)
{
    unlock_async((GpioLockType)type, duration_ms, play_voice);
}

static void unlock_async(GpioLockType type, int duration_ms, int play_voice)
{
    int idx = lock_busy_idx(type);
    /* 锁正在开着时不重复开锁 */
    if (s_lock_busy[idx])
        return;
    s_lock_busy[idx] = 1;

    UnlockArg *ua = malloc(sizeof(UnlockArg));
    if (!ua) { s_lock_busy[idx] = 0; return; }
    ua->type        = type;
    ua->duration_ms = duration_ms;
    ua->play_voice  = play_voice;
    pthread_t t;
    if (pthread_create(&t, NULL, unlock_thread, ua) != 0) {
        s_lock_busy[idx] = 0;
        free(ua);
        return;
    }
    pthread_detach(t);
}

/* =========================================================
 *  安防错误计数
 *  规格：5分钟内连续10次错误 → 触发，保护期2分钟
 *  SafeMode=OFF 时直接忽略，不累计也不触发
 * ========================================================= */
#define SECURITY_ERROR_MAX      10

/* 错误窗口 / 保护时长：锁死模式 120s，报警模式 60s */
#define SECURITY_TIME_LOCK_MS   (120 * 1000)
#define SECURITY_TIME_ALARM_MS  (60  * 1000)

static int s_error_count = 0;

static inline unsigned int security_time_ms(void)
{
    return (AppUserConfigGet()->SafeMode == APP_SAFE_MODE_LOCK)
           ? SECURITY_TIME_LOCK_MS : SECURITY_TIME_ALARM_MS;
}

/* --- 报警音链式播放（on_end 回调自动续播）--- */
static void alarm_sound_end_cb(void)
{
    if (SvcTimerActive(TMR_SECURITY_TRIGGER))
        SvcVoicePlay(VOICE_Bi4, VOICE_VOL_DEFAULT, NULL, alarm_sound_end_cb);
}

/* --- 报警灯闪烁（500ms 切换，保护期内持续）--- */
static void alarm_light_flash_cb(void *arg)
{
    static int s_light_on = 1;
    (void)arg;
    if (SvcTimerActive(TMR_SECURITY_TRIGGER)) {
        s_light_on = !s_light_on;
        DrvGpioKeypadLightSet(s_light_on);
        SvcTimerSet(TMR_ALARM_LIGHT, 500, alarm_light_flash_cb, NULL);
    } else {
        /* 保护期结束，恢复灯光：按对讲状态恢复 Key1/Key2 */
        int on = SvcTimerActive(TMR_INTERCOM_WATCHDOG) ? 1 : 0;
        DrvGpioKey1LightSet(on);
        DrvGpioKey2LightSet(on);
    }
}

static void security_error_reset_cb(void *arg)
{
    (void)arg;
    s_error_count = 0;
}

static void security_error_reset(void)
{
    s_error_count = 0;
    SvcTimerStop(TMR_SECURITY_ERROR);
}

/* 公开给 app_keypad.c 的包装 */
void AppCardSecurityErrorReset(void)  { security_error_reset(); }
void AppCardSecurityErrorUpdate(void) { security_error_update(); }

static void security_error_update(void)
{
    /* 安防关闭时不累计错误 */
    if (AppUserConfigGet()->SafeMode == APP_SAFE_MODE_OFF)
        return;

    /* 第一次错误时启动统计窗口计时器（到期自动清零）*/
    if (s_error_count == 0)
        SvcTimerSet(TMR_SECURITY_ERROR, security_time_ms(), security_error_reset_cb, NULL);

    LOG_W("SecurityErrorCount=%d", s_error_count + 1);
    if (++s_error_count >= SECURITY_ERROR_MAX) {
        s_error_count = 0;
        SvcTimerStop(TMR_SECURITY_ERROR);
        SvcTimerSet(TMR_SECURITY_TRIGGER, security_time_ms(), NULL, NULL);

        /* 报警灯闪烁（锁死/报警模式共用，首次 200ms 后开始）*/
        SvcTimerSet(TMR_ALARM_LIGHT, 200, alarm_light_flash_cb, NULL);

        /* 报警模式：蜂鸣器链式播放，直到保护期结束 */
        if (AppUserConfigGet()->SafeMode == APP_SAFE_MODE_ALARM)
            SvcVoicePlay(VOICE_Bi4, VOICE_VOL_DEFAULT, NULL, alarm_sound_end_cb);

        /* 通知其他模块（app_intercom.c 订阅后自动呼叫室内机）*/
        EventBusPublish(EVT_SYSTEM_SECURITY_TRIGGERED, NULL, 0);
        LOG_W("Security triggered! mode=%d", (int)AppUserConfigGet()->SafeMode);
    }
}

/* =========================================================
 *  刷卡指示灯闪烁
 * ========================================================= */
static void card_light_off_cb(void *arg)
{
    (void)arg;
    DrvGpioCardLightSet(0);
}

static void card_light_flash(void)
{
    DrvGpioCardLightSet(1);
    SvcTimerSet(TMR_CARD_LIGHT, 200, card_light_off_cb, NULL);
}

/* =========================================================
 *  去重过滤（同一张卡 1s 内只处理一次）
 * ========================================================= */
static int filter_duplicate(const char *uid4)
{
    static char last_uid[4] = {0};
    static struct timespec last_time = {0};

    unsigned long long diff = UtilsDiffMs(&last_time);
    if (diff < 1000)
        return -1;  /* 频率过快，丢弃 */

    UtilsGetTime(&last_time);

    if (memcmp(last_uid, uid4, 4) == 0)
        return 0;   /* 同一张卡，但间隔超过 1s，允许通过 */

    memcpy(last_uid, uid4, 4);
    return 0;
}

/* =========================================================
 *  刷卡业务处理
 * ========================================================= */
void AppCardHandle(char *raw_uid4)
{
    /* 去重 */
    if (filter_duplicate(raw_uid4) == -1)
        return;

    /* 安防触发保护期内不响应 */
    if (SvcTimerActive(TMR_SECURITY_TRIGGER))
        return;

    card_light_flash();

    /* 补充第 5 字节校验（RC522 只读 4 字节，第 5 字节 = 前 4 字节异或）*/
    char data[CARD_DATA_LEN];
    memcpy(data, raw_uid4, 4);
    data[4] = (char)(data[0] ^ data[1] ^ data[2] ^ data[3]);

    LOG_D("CARD: [%02x %02x %02x %02x %02x]",
          (unsigned char)data[0], (unsigned char)data[1],
          (unsigned char)data[2], (unsigned char)data[3],
          (unsigned char)data[4]);

    int card_idx = AppCardSearch(data);
    if (card_idx != -1)
        LOG_D("card[%d] found, code=%.4s", card_idx, s_deck.Deck[card_idx].Code);

    /* ---- 添加模式 ---- */
    if (SvcTimerActive(TMR_ADD_CARD)) {
        int ret = 0;
        if (card_idx == -1) {
            ret = AppCardAdd(-1, data, CARD_PERM_LOCK);
            if (ret) {
                LOG_I("add card succeed");
                SvcTimerRefresh(TMR_ADD_CARD, 30000);
            }
        }
        LOG_I("add card %s", ret ? "succeed" : "fail");
        SvcVoicePlaySimple(ret ? VOICE_Bi2 : VOICE_Bi4, VOICE_VOL_DEFAULT);

        /* 回传结果到室内机（arg1=1 表示成功，arg1=9 表示失败，室内机据此刷新显示）*/
        SvcNetManageSendShort(NET_MGR_ADD_CARD, (unsigned char)(ret ? 1 : 9), 0);
        return;
    }

    /* ---- 删除模式 ---- */
    if (SvcTimerActive(TMR_DEL_CARD)) {
        if (card_idx != -1) {
            if (AppCardSetPerm(card_idx, 0)) {
                LOG_I("del card[%d] succeed", card_idx);
                SvcVoicePlaySimple(VOICE_Bi3, VOICE_VOL_DEFAULT);
                return;
            }
        }
        SvcVoicePlaySimple(VOICE_Bi4, VOICE_VOL_DEFAULT);
        LOG_W("del card fail");
        return;
    }

    /* ---- 修改密码模式 ---- */
    if (SvcTimerActive(TMR_MODIFY_CODE_CARD)) {
        if (card_idx != -1) {
            AppKeypadSetModifyCardIdx(card_idx);   /* 告知键盘当前卡索引 */
            SvcTimerRefresh(TMR_MODIFY_CODE_CARD, 30000);
            SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
        }
        return;
    }

    /* ---- 正常开锁 ---- */
    if (card_idx != -1) {
        AppUserConfig *cfg = AppUserConfigGet();

        /* 卡+码模式：先刷卡，等待键盘输入密码 */
        if (cfg->LockWay == UNLOCK_WAY_CARD_AND_CODE) {
            if (!SvcTimerActive(TMR_CODE_CARD_UNLOCK)) {
                s_code_card_idx = card_idx;
                SvcTimerSet(TMR_CODE_CARD_UNLOCK, 30000, NULL, NULL);
                SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
                LOG_I("CardAndCode: waiting for keypad code, card=%d", card_idx);
            }
            return;
        }

        security_error_reset();
        SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);

        char perm = s_deck.Deck[card_idx].Perm;

        /* 第一路开锁传 play_voice=1，后续传 0，避免重复播音 */
        int first = 1;
        if (perm & CARD_PERM_LOCK) {
            unlock_async(GPIO_LOCK_DOOR, cfg->UnlockTime * 1000, first);
            first = 0;
        }
        if (perm & CARD_PERM_GATE)
            unlock_async(GPIO_LOCK_GATE, cfg->UngateTime * 1000, first);

        LOG_I("card[%d] verify OK, perm=%d unlock=%ds ungate=%ds",
              card_idx, (int)(unsigned char)perm,
              cfg->UnlockTime, cfg->UngateTime);
        return;
    }

    /* ---- 未知卡 ---- */
    SvcVoicePlaySimple(VOICE_Bi4, VOICE_VOL_DEFAULT);
    security_error_update();
    LOG_W("card verify FAIL");
}

/* =========================================================
 *  远程开锁（EVT_NET_UNLOCK_CMD）
 * ========================================================= */
static void on_net_unlock(EventId id, const void *data, size_t len)
{
    (void)id;
    if (len < sizeof(NetUnlockArg)) return;
    const NetUnlockArg *ua = (const NetUnlockArg *)data;

    AppUserConfig *cfg = AppUserConfigGet();

    /* unlock_time=0 时使用配置时长 */
    int unlock_ms = (ua->unlock_time > 0)
                    ? (ua->unlock_time * 1000) : (cfg->UnlockTime * 1000);
    int ungate_ms = cfg->UngateTime * 1000;

    SvcVoicePlaySimple(VOICE_Bi1, VOICE_VOL_DEFAULT);
    if (cfg->UnlockVoiceEn) {
        int lang = (ua->language_idx < APP_LANG_TOTAL)
                   ? (int)ua->language_idx : (int)cfg->Language;
        int vi = (int)VOICE_UnlockEng + lang;
        if (vi < VOICE_TOTAL)
            SvcVoicePlaySimple((VoiceId)vi, VOICE_VOL_DEFAULT);
    }

    /* lock_type: bit0=门锁，bit1=门闸；0=门锁（默认）*/
    int do_door = (ua->lock_type == 0) || (ua->lock_type & 0x01);
    int do_gate = (ua->lock_type & 0x02);

    if (do_door) unlock_async(GPIO_LOCK_DOOR, unlock_ms, 0);
    if (do_gate) unlock_async(GPIO_LOCK_GATE, ungate_ms, 0);

    LOG_I("NET_UNLOCK type=%d time=%dms lang=%d",
          ua->lock_type, unlock_ms, ua->language_idx);
}

/* =========================================================
 *  初始化
 * ========================================================= */
int AppCardInit(void)
{
    AppCardDeckInit();
    DrvCardSetCallback(AppCardHandle);
    EventBusSubscribe(EVT_NET_UNLOCK_CMD, on_net_unlock);
    LOG_I("init ok, DeckSize=%d", (int)(unsigned char)s_deck.DeckSize);
    return 0;
}
