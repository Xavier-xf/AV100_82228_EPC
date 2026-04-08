/**
 * @file    app_user_config.c
 * @brief   用户配置管理（开锁时长、语言、开锁提示音等）
 *
 * 支持出厂默认配置文件（UserDefaultConfig.cfg）覆盖内置默认值，
 * 再从用户配置文件（UserConfig.cfg）加载运行时配置。
 */
#define LOG_TAG "UserConfig"
#include "log.h"

#include "app_user_config.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

/* =========================================================
 *  内部数据
 * ========================================================= */
static AppUserConfig s_conf;

static AppUserConfig s_conf_default = {
    .AdminCode      = {'9', '9', '9', '9', '9', '9', '\0'},
    .UnlockCode     = {'1', '2', '3', '4', '5', '6', '\0'},
    .UngateCode     = {'4', '5', '6', '7', '8', '9', '\0'},
    .PublicUnlockEn = 1,
    .UnlockVoiceEn  = 1,
    .UnlockTime     = USER_CONFIG_DEFAULT_UNLOCK_TIME,
    .UngateTime     = USER_CONFIG_DEFAULT_UNGATE_TIME,
    .NumKeyLightTime= 0,
    .Language       = APP_LANG_POLISH,
    .LockWay        = UNLOCK_WAY_CARD_OR_CODE,
    .SafeMode       = APP_SAFE_MODE_OFF,
};

/* =========================================================
 *  公共接口
 * ========================================================= */

AppUserConfig *AppUserConfigGet(void)
{
    return &s_conf;
}

AppUserConfig *AppUserDefaultConfigGet(void)
{
    return &s_conf_default;
}

int AppUserConfigSave(void)
{
    int fd = open(USER_CONFIG_PATH, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        LOG_E("open %s fail", USER_CONFIG_PATH);
        return 0;
    }
    write(fd, &s_conf, sizeof(AppUserConfig));
    close(fd);
    system("fsync -d " USER_CONFIG_PATH);
    return 1;
}

int AppUserDefaultConfigSave(void)
{
    int fd = open(USER_DEFAULT_CONFIG_PATH, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        LOG_E("open %s fail", USER_DEFAULT_CONFIG_PATH);
        return 0;
    }
    write(fd, &s_conf_default, sizeof(AppUserConfig));
    close(fd);
    system("fsync -d " USER_DEFAULT_CONFIG_PATH);
    return 1;
}

int AppUserConfigReset(void)
{
    LOG_I("reset to default");
    AppLanguage lang = s_conf.Language;
    s_conf = s_conf_default;
    s_conf.Language = lang;   /* 保留语言设置 */
    AppUserConfigSave();
    return 1;
}

int AppUserConfigInit(void)
{
    /* Step 1: 从出厂默认配置文件覆盖内置默认值（部分字段）*/
    int dfd = open(USER_DEFAULT_CONFIG_PATH, O_RDONLY);
    if (dfd >= 0) {
        AppUserConfig tmp;
        if (read(dfd, &tmp, sizeof(tmp)) == sizeof(tmp)) {
            s_conf_default.Language    = tmp.Language;
            s_conf_default.UnlockTime  = tmp.UnlockTime;
            s_conf_default.UngateTime  = tmp.UngateTime;
            memcpy(s_conf_default.UnlockCode, tmp.UnlockCode,
                   sizeof(tmp.UnlockCode));
            memcpy(s_conf_default.UngateCode, tmp.UngateCode,
                   sizeof(tmp.UngateCode));
        }
        close(dfd);
    }

    /* Step 2: 从用户配置文件加载 */
    int fd = open(USER_CONFIG_PATH, O_RDONLY);
    if (fd < 0) {
        LOG_W("%s not found, using default", USER_CONFIG_PATH);
        s_conf = s_conf_default;
        AppUserConfigSave();
        return 0;
    }
    read(fd, &s_conf, sizeof(AppUserConfig));
    close(fd);

    LOG_I("loaded: lang=%d unlock=%ds ungate=%ds voice=%d",
          s_conf.Language, s_conf.UnlockTime, s_conf.UngateTime,
          s_conf.UnlockVoiceEn);
    return 1;
}
