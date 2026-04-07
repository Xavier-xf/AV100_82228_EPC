/**
 * @file    app_user_config.h
 * @brief   用户配置（开锁时长、语言、开锁提示音等）
 *
 * 移植自旧版 UserConfig.h / UserConfig.c，仅保留与新版功能相关的字段。
 * 硬件看门狗相关代码已移至 drv_watchdog.c，不在此重复。
 */
#ifndef _APP_USER_CONFIG_H_
#define _APP_USER_CONFIG_H_

#include <stdint.h>

#define USER_CONFIG_PATH         "/etc/config/UserConfig.cfg"
#define USER_DEFAULT_CONFIG_PATH "/etc/config/UserDefaultConfig.cfg"

#define ADMIN_CODE_LEN        6
#define UNLOCK_CODE_MAX_LEN   6
#define UNLOCK_CODE_MIN_LEN   4

/* 默认开锁时长（秒），与旧版 DEFAULT_UNLOCK_TIME 一致 */
#define USER_CONFIG_DEFAULT_UNLOCK_TIME  5
#define USER_CONFIG_DEFAULT_UNGATE_TIME  5

/* ---- 解锁方式 ---- */
typedef enum {
    UNLOCK_WAY_CARD_ONLY    = 0,  /* 仅刷卡    */
    UNLOCK_WAY_CARD_OR_CODE = 1,  /* 刷卡或密码 */
    UNLOCK_WAY_CARD_AND_CODE= 2,  /* 刷卡+密码  */
} AppUnlockWay;

/* ---- 安防模式 ---- */
typedef enum {
    APP_SAFE_MODE_OFF   = 0,
    APP_SAFE_MODE_LOCK  = 1,
    APP_SAFE_MODE_ALARM = 2,
} AppSafeMode;

/* ---- 语言（与旧版 Language 枚举和 VOICE_UnlockEng 偏移量一一对应）---- */
typedef enum {
    APP_LANG_ENGLISH  = 0,
    APP_LANG_CHINESE  = 1,
    APP_LANG_GERMAN   = 2,
    APP_LANG_HEBREW   = 3,
    APP_LANG_POLISH   = 4,
    APP_LANG_PORTUGAL = 5,
    APP_LANG_SPAIN    = 6,
    APP_LANG_FRENCH   = 7,
    APP_LANG_JAPANESE = 8,
    APP_LANG_ITALIAN  = 9,
    APP_LANG_DUTCH    = 10,
    APP_LANG_SLOVAKIA = 11,
    APP_LANG_TOTAL,
} AppLanguage;

/* ---- 用户配置结构（与旧版二进制兼容）---- */
typedef struct {
    char          AdminCode[ADMIN_CODE_LEN + 1];       /* 管理员密码 */
    char          UnlockCode[UNLOCK_CODE_MAX_LEN + 1]; /* 开锁密码   */
    char          UngateCode[UNLOCK_CODE_MAX_LEN + 1]; /* 开闸密码   */
    char          PublicUnlockEn;  /* 公开开锁使能（密码本功能）*/
    char          UnlockVoiceEn;   /* 开锁提示音使能            */
    int           UnlockTime;      /* 门锁开启时长（秒）         */
    int           UngateTime;      /* 闸机开启时长（秒）         */
    int           NumKeyLightTime; /* 按键背光时长（秒，0=常亮） */
    AppLanguage   Language;        /* 提示语言                  */
    AppUnlockWay  LockWay;         /* 解锁方式                  */
    AppSafeMode   SafeMode;        /* 安防模式                  */
} AppUserConfig;

/**
 * @brief 获取当前用户配置指针（内部静态存储，勿 free）
 */
AppUserConfig *AppUserConfigGet(void);

/**
 * @brief 获取出厂默认配置指针
 */
AppUserConfig *AppUserDefaultConfigGet(void);

/**
 * @brief 保存当前配置到 /etc/config/UserConfig.cfg
 * @return 1=成功，0=失败
 */
int AppUserConfigSave(void);

/**
 * @brief 恢复出厂默认配置（保留语言设置）
 * @return 1=成功
 */
int AppUserConfigReset(void);

/**
 * @brief 初始化用户配置（从文件加载，不存在则使用默认值）
 * @return 1=从文件加载，0=使用默认值
 */
int AppUserConfigInit(void);

#endif /* _APP_USER_CONFIG_H_ */
