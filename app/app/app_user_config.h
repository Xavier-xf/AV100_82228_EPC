/**
 * @file    app_user_config.h
 * @brief   用户配置（开锁时长、密码、语言、安防模式等）
 *
 * 配置以二进制结构体持久化在两个文件中：
 *   /etc/config/UserConfig.cfg        — 运行时用户配置（随用户修改变化）
 *   /etc/config/UserDefaultConfig.cfg — 出厂默认配置（部分字段可通过管理员菜单修改）
 *
 * 初始化时先用出厂默认覆盖内置默认值（语言/时长/密码），
 * 再从运行时配置文件加载，文件不存在则按内置默认值新建。
 */
#ifndef _APP_USER_CONFIG_H_
#define _APP_USER_CONFIG_H_

#include <stdint.h>

#define USER_CONFIG_PATH         "/etc/config/UserConfig.cfg"
#define USER_DEFAULT_CONFIG_PATH "/etc/config/UserDefaultConfig.cfg"

#define ADMIN_CODE_LEN        6   /* 管理员密码长度（固定 6 位）           */
#define UNLOCK_CODE_MAX_LEN   6   /* 密码最大长度                          */
#define UNLOCK_CODE_MIN_LEN   4   /* 密码最小长度                          */

#define USER_CONFIG_DEFAULT_UNLOCK_TIME  5  /* 默认门锁开启时长（秒）      */
#define USER_CONFIG_DEFAULT_UNGATE_TIME  5  /* 默认闸机开启时长（秒）      */

/* =========================================================
 *  解锁方式
 * ========================================================= */
typedef enum {
    UNLOCK_WAY_CARD_ONLY     = 0,  /* 仅刷卡（密码无效）                  */
    UNLOCK_WAY_CARD_OR_CODE  = 1,  /* 刷卡或密码均可开锁                  */
    UNLOCK_WAY_CARD_AND_CODE = 2,  /* 刷卡后还需输密码（双因子）          */
} AppUnlockWay;

/* =========================================================
 *  安防模式
 * ========================================================= */
typedef enum {
    APP_SAFE_MODE_OFF   = 0,  /* 关闭安防                                  */
    APP_SAFE_MODE_LOCK  = 1,  /* 锁定模式：错误次数超限后锁定键盘          */
    APP_SAFE_MODE_ALARM = 2,  /* 报警模式：错误次数超限后触发安防事件      */
} AppSafeMode;

/* =========================================================
 *  语言索引
 *  与语音文件偏移量一一对应（VOICE_UnlockEng + Language = 对应语言提示音）
 * ========================================================= */
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

/* =========================================================
 *  用户配置结构体
 *
 *  ⚠️ 字段顺序和大小影响二进制文件格式，不可随意增删或改变类型。
 *     若需新增字段，追加在末尾并更新版本号校验。
 * ========================================================= */
typedef struct {
    char         AdminCode[ADMIN_CODE_LEN + 1];       /* 管理员密码（'\0' 结尾）                */
    char         UnlockCode[UNLOCK_CODE_MAX_LEN + 1]; /* 公共开锁密码（'\0' 结尾）              */
    char         UngateCode[UNLOCK_CODE_MAX_LEN + 1]; /* 公共开闸密码（'\0' 结尾）              */
    char         PublicUnlockEn;  /* 公共密码使能：1=允许用公共密码开锁     */
    char         UnlockVoiceEn;   /* 开锁提示音使能：1=开锁时播放语音提示   */
    int          UnlockTime;      /* 门锁继电器吸合时长（秒）               */
    int          UngateTime;      /* 闸机继电器吸合时长（秒）               */
    int          NumKeyLightTime; /* 键盘背光时长（秒），0 = 夜间常亮       */
    AppLanguage  Language;        /* 语音提示语言                          */
    AppUnlockWay LockWay;         /* 解锁方式                              */
    AppSafeMode  SafeMode;        /* 安防模式                              */
} AppUserConfig;

/* =========================================================
 *  接口
 * ========================================================= */

/** @brief 获取当前用户配置指针（内部静态存储，勿 free）*/
AppUserConfig *AppUserConfigGet(void);

/** @brief 获取出厂默认配置指针（内部静态存储，勿 free）*/
AppUserConfig *AppUserDefaultConfigGet(void);

/** @brief 将当前配置持久化到 UserConfig.cfg，返回 1=成功 0=失败 */
int AppUserConfigSave(void);

/** @brief 将出厂默认配置持久化到 UserDefaultConfig.cfg，返回 1=成功 0=失败 */
int AppUserDefaultConfigSave(void);

/** @brief 恢复出厂默认配置（语言设置保留），返回 1=成功 */
int AppUserConfigReset(void);

/** @brief 初始化：先加载出厂默认文件，再加载运行时配置文件；文件不存在则创建
 *  @return 1=从文件加载  0=使用内置默认值 */
int AppUserConfigInit(void);

#endif /* _APP_USER_CONFIG_H_ */
