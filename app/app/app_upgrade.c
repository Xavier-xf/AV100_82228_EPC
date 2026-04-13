/**
 * @file    app_upgrade.c
 * @brief   OTA 固件升级业务完整实现
 *
 *   AppUpgradeHandleLongPack  → 快速写文件，不阻塞
 *   AppUpgradeHandleCtrlPack  → 收到执行命令后，在独立线程中先发应答再升级
 *   升级线程中先关看门狗，再执行升级，保证顺序
 *   状态机 (IDLE/RECEIVING/PROCESSING/DONE/ERROR) 替代裸 static 变量
 */
#define LOG_TAG "Upgrade"
#include "log.h"

#include "app_upgrade.h"
#include "event_bus.h"
#include "svc_network.h"
#include "svc_voice.h"
#include "drv_watchdog.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

/* =========================================================
 *  升级包内部结构常量
 * ========================================================= */
#define UPGRADE_TMP_PATH   "/tmp/"
#define UPGRADE_RAR_DIR    "image/"
#define UPGRADE_PKG_RAR    "image.tar.gz"
#define UPGRADE_PKG_SHA1   "image.sha1"

/* =========================================================
 *  内部状态
 * ========================================================= */
typedef struct {
    pthread_mutex_t   lock;
    AppUpgradeState   state;
    int               pkt_sum;      /* 已收到的包序号累计 */
    const char       *pack_file;    /* 当前升级包路径 */
} AppUpgradeCtx;

static AppUpgradeCtx s_upg = {
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .state     = UPGRADE_STATE_IDLE,
    .pkt_sum   = 0,
    .pack_file = UPGRADE_PACK_NET,
};

static AppUpgradeState get_state(void)
{
    pthread_mutex_lock(&s_upg.lock);
    AppUpgradeState v = s_upg.state;
    pthread_mutex_unlock(&s_upg.lock);
    return v;
}

static void set_state(AppUpgradeState st)
{
    pthread_mutex_lock(&s_upg.lock);
    s_upg.state = st;
    pthread_mutex_unlock(&s_upg.lock);
}

/* =========================================================
 *  网络应答
 * ========================================================= */
static void send_reply(uint8_t dst_dev, uint8_t arg1, uint8_t arg2)
{
    SvcNetworkUpgradeReply(dst_dev, arg1, arg2);
    LOG_D("reply dev=0x%02X arg1=%d arg2=%d", dst_dev, arg1, arg2);
}

/* =========================================================
 *  Step1: 接收数据包，追加写入升级包文件
 *  包序号从 1 开始，必须连续递增，否则判定为乱序失败
 * ========================================================= */
static int recv_pack(uint32_t index, uint32_t len, const uint8_t *data)
{
    pthread_mutex_lock(&s_upg.lock);

    if (index == 1) {
        s_upg.pkt_sum = 0;
        pthread_mutex_unlock(&s_upg.lock);

        LOG_I("*** RECEIVE START ***");
        system("rm -f "   UPGRADE_PACK_NET);
        system("touch "   UPGRADE_PACK_NET);
        system("chmod 777 " UPGRADE_PACK_NET);
        system("rm -rf "  UPGRADE_TMP_PATH UPGRADE_RAR_DIR);

        pthread_mutex_lock(&s_upg.lock);
        s_upg.state     = UPGRADE_STATE_RECEIVING;
        s_upg.pkt_sum   = 0;
        s_upg.pack_file = UPGRADE_PACK_NET;
    }

    int expected = s_upg.pkt_sum + 1;
    pthread_mutex_unlock(&s_upg.lock);

    if ((int)index != expected) {
        LOG_E("packet order error: expect=%d got=%u", expected, index);
        set_state(UPGRADE_STATE_ERROR);
        return 0;
    }

    FILE *fp = fopen(UPGRADE_PACK_NET, "ab");
    if (!fp) {
        LOG_E("fopen fail");
        set_state(UPGRADE_STATE_ERROR);
        return 0;
    }
    int written = (int)fwrite(data, 1, (size_t)len, fp);
    fclose(fp);

    if ((uint32_t)written != len) {
        LOG_E("write error: want=%u got=%d", len, written);
        set_state(UPGRADE_STATE_ERROR);
        return 0;
    }

    pthread_mutex_lock(&s_upg.lock);
    s_upg.pkt_sum = (int)index;
    pthread_mutex_unlock(&s_upg.lock);

    LOG_D("pack %u ok (%u bytes)", index, len);
    return 1;
}

/* =========================================================
 *  Step2: 解压外层 .update 包，验证文件列表完整性
 * ========================================================= */
static const char *s_required_files[] = {
    UPGRADE_RAR_DIR, UPGRADE_PKG_RAR, UPGRADE_PKG_SHA1
};
#define REQUIRED_CNT  (int)(sizeof(s_required_files)/sizeof(s_required_files[0]))

static bool step1_decompress_check(const char *pack_path)
{
    LOG_I("--- step1: decompress & verify file list ---");

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "tar -xzvf %s -C %s 2>&1; rm -f %s",
             pack_path, UPGRADE_TMP_PATH, pack_path);

    FILE *pf = popen(cmd, "r");
    if (!pf) { LOG_E("popen tar fail"); return false; }

    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), pf)) {
        for (int i = 0; i < REQUIRED_CNT; i++) {
            if (!(found & (1 << i)) && strstr(line, s_required_files[i]))
                found |= (1 << i);
        }
    }
    int tar_ret = pclose(pf);
    if (tar_ret != 0) {
        LOG_E("tar decompress returned %d", tar_ret);
        return false;
    }

    int count = 0;
    for (int v = found; v; v &= v - 1) count++;
    LOG_I("found %d/%d required files (mask=0x%X)", count, REQUIRED_CNT, found);
    return (count == REQUIRED_CNT);
}

/* =========================================================
 *  Step3: 校验 image.tar.gz 的 SHA1
 * ========================================================= */
static bool step2_verify_sha1(void)
{
    LOG_I("--- step2: verify SHA1 ---");

    char cmd[256], line[256];
    char current_hash[128] = {0};
    char correct_hash[256] = {0};

    snprintf(cmd, sizeof(cmd), "sha1sum %s%s%s",
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR, UPGRADE_PKG_RAR);
    FILE *pf = popen(cmd, "r");
    if (!pf) { LOG_E("sha1sum fail"); return false; }
    if (fgets(line, sizeof(line), pf)) {
        char *sp = strchr(line, ' ');
        if (sp) {
            int n = (int)(sp - line);
            if (n > 127) n = 127;
            strncpy(current_hash, line, (size_t)n);
            current_hash[n] = '\0';
        }
    }
    pclose(pf);

    char sha1_path[256];
    snprintf(sha1_path, sizeof(sha1_path), "%s%s%s",
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR, UPGRADE_PKG_SHA1);
    FILE *fp = fopen(sha1_path, "r");
    if (!fp) { LOG_E("open sha1 file fail"); return false; }
    fscanf(fp, "%255s", correct_hash);
    fclose(fp);

    bool ok = (strlen(correct_hash) > 0) &&
              (strncmp(correct_hash, current_hash, strlen(correct_hash)) == 0);
    LOG_I("SHA1 %s | current=%s expected=%s",
          ok ? "PASS" : "FAIL", current_hash, correct_hash);
    return ok;
}

/* =========================================================
 *  Step4: 解压 image.tar.gz，执行 update.sh
 * ========================================================= */
static bool step3_flash(void)
{
    LOG_I("--- step3: flash image ---");

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "tar -zxvf %s%s%s -C %s 2>&1; rm -rf %s%s",
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR, UPGRADE_PKG_RAR,
             UPGRADE_TMP_PATH,
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR);

    FILE *pf = popen(cmd, "r");
    if (!pf) { LOG_E("decompress image fail"); return false; }
    char line[256];
    while (fgets(line, sizeof(line), pf)) ;
    int pret = pclose(pf);
    if (pret != 0) {
        LOG_E("decompress image returned %d", pret);
        return false;
    }

    if (access("/tmp/update.sh", F_OK) != 0) {
        LOG_E("/tmp/update.sh not found");
        return false;
    }

    LOG_I("executing update.sh (system will reboot)");
    fflush(stdout);

    int ret = system("/tmp/update.sh");
    if (ret != 0) {
        LOG_E("update.sh returned %d", ret);
        return false;
    }
    return true;
}

/* =========================================================
 *  升级主流程（独立线程执行，不阻塞事件回调）
 * ========================================================= */
typedef struct {
    uint8_t  sender_dev;
    char     pack_path[256];
} UpgradeThreadArg;

static void *upgrade_thread(void *arg)
{
    UpgradeThreadArg *ta = (UpgradeThreadArg *)arg;
    uint8_t sender = ta->sender_dev;
    char pack_path[256];
    strncpy(pack_path, ta->pack_path, sizeof(pack_path) - 1);
    pack_path[sizeof(pack_path) - 1] = '\0';
    free(ta);

    LOG_I("upgrade thread start");
    set_state(UPGRADE_STATE_PROCESSING);

    SvcVoicePlaySimple(VOICE_Unlock16k, 90);
    sleep(2);

    DrvWdtClose();
    LOG_I("watchdog disabled");

    bool ok = step1_decompress_check(pack_path)
           && step2_verify_sha1()
           && step3_flash();

    if (!ok) {
        LOG_E("*** UPGRADE FAILED ***");
        set_state(UPGRADE_STATE_ERROR);
        send_reply(sender, UPGRADE_REPLY_FAIL, 1);
        system("rm -f "  UPGRADE_PACK_NET);
        system("rm -rf " UPGRADE_TMP_PATH UPGRADE_RAR_DIR);
        system("rm -f /tmp/update.sh");
        DrvWdtOpen(10);
        DrvWdtFeed();
        set_state(UPGRADE_STATE_IDLE);
    } else {
        set_state(UPGRADE_STATE_DONE);
    }

    LOG_I("upgrade thread exit");
    return NULL;
}

static void start_upgrade_thread(uint8_t sender_dev, const char *pack_path)
{
    UpgradeThreadArg *ta = (UpgradeThreadArg *)malloc(sizeof(UpgradeThreadArg));
    if (!ta) { LOG_E("malloc fail"); return; }
    ta->sender_dev = sender_dev;
    strncpy(ta->pack_path, pack_path, sizeof(ta->pack_path) - 1);
    ta->pack_path[sizeof(ta->pack_path) - 1] = '\0';

    pthread_t tid;
    if (pthread_create(&tid, NULL, upgrade_thread, ta) != 0) {
        LOG_E("create upgrade thread fail");
        free(ta);
        return;
    }
    pthread_detach(tid);
    LOG_I("upgrade thread started");
}

/* =========================================================
 *  取消升级（清理临时文件）
 * ========================================================= */
static void cancel_upgrade(void)
{
    LOG_I("cancelled, cleaning temp files");
    system("rm -f "  UPGRADE_PACK_NET);
    system("rm -rf " UPGRADE_TMP_PATH UPGRADE_RAR_DIR);
    system("rm -f /tmp/update.sh");
    set_state(UPGRADE_STATE_IDLE);
    pthread_mutex_lock(&s_upg.lock);
    s_upg.pkt_sum = 0;
    pthread_mutex_unlock(&s_upg.lock);
}

/* =========================================================
 *  公开接口
 * ========================================================= */
int AppUpgradeHandleLongPack(uint8_t sender_dev,
                              uint32_t index, uint32_t data_len,
                              const uint8_t *data)
{
    AppUpgradeState cur = get_state();
    if (cur == UPGRADE_STATE_PROCESSING || cur == UPGRADE_STATE_DONE) {
        LOG_W("in processing, ignore long pack");
        return 0;
    }

    LOG_D("long pack index=%u len=%u", index, data_len);

    if (recv_pack(index, data_len, data) == 0) {
        send_reply(sender_dev, UPGRADE_REPLY_FAIL, 1);
        return 0;
    }
    return 1;
}

void AppUpgradeHandleCtrlPack(uint8_t sender_dev,
                               uint8_t ctrl_arg0, uint8_t ctrl_arg1)
{
    LOG_D("ctrl dev=0x%02X arg0=0x%02X arg1=0x%02X",
          sender_dev, ctrl_arg0, ctrl_arg1);

    if (ctrl_arg0 & 0x01) {
        send_reply(sender_dev, UPGRADE_REPLY_ONLINE, 1);
        return;
    }

    if (ctrl_arg0 & 0x02) {
        if (ctrl_arg1 & 0x01) {
            AppUpgradeState cur = get_state();
            if (cur == UPGRADE_STATE_RECEIVING || cur == UPGRADE_STATE_IDLE) {
                const char *pack_path;
                pthread_mutex_lock(&s_upg.lock);
                pack_path = s_upg.pack_file;
                pthread_mutex_unlock(&s_upg.lock);

                if (access(pack_path, F_OK) != 0) {
                    LOG_E("pack not found: %s", pack_path);
                    send_reply(sender_dev, UPGRADE_REPLY_FAIL, 1);
                    return;
                }

                send_reply(sender_dev, UPGRADE_REPLY_EXECUTE, 1);
                start_upgrade_thread(sender_dev, pack_path);
            } else {
                LOG_W("not in receiving state (%d), ignore", cur);
                send_reply(sender_dev, UPGRADE_REPLY_FAIL, 1);
            }
            return;
        }

        if (ctrl_arg1 & 0x02) {
            cancel_upgrade();
            return;
        }
    }

    LOG_W("unknown ctrl arg0=0x%02X arg1=0x%02X", ctrl_arg0, ctrl_arg1);
}

int AppUpgradeFromSD(void)
{
    if (get_state() != UPGRADE_STATE_IDLE) {
        LOG_W("already in progress");
        return 0;
    }
    if (access(UPGRADE_PACK_SD, F_OK) != 0) {
        LOG_W("SD pack not found: %s", UPGRADE_PACK_SD);
        return 0;
    }

    LOG_I("SD card upgrade: %s", UPGRADE_PACK_SD);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cp %s %s", UPGRADE_PACK_SD, UPGRADE_PACK_NET);
    if (system(cmd) != 0) {
        LOG_E("cp SD pack fail");
        return 0;
    }

    pthread_mutex_lock(&s_upg.lock);
    s_upg.pack_file = UPGRADE_PACK_NET;
    s_upg.state     = UPGRADE_STATE_RECEIVING;
    pthread_mutex_unlock(&s_upg.lock);

    send_reply(0, UPGRADE_REPLY_EXECUTE, 1);
    start_upgrade_thread(0, UPGRADE_PACK_NET);
    return 1;
}

AppUpgradeState AppUpgradeGetState(void)
{
    return get_state();
}

int AppUpgradeInit(void)
{
    if (access(UPGRADE_PACK_NET, F_OK) == 0) {
        LOG_W("stale net pack found, removing");
        system("rm -f " UPGRADE_PACK_NET);
    }
    system("rm -rf " UPGRADE_TMP_PATH UPGRADE_RAR_DIR);
    system("rm -f /tmp/update.sh");

    pthread_mutex_lock(&s_upg.lock);
    s_upg.state     = UPGRADE_STATE_IDLE;
    s_upg.pkt_sum   = 0;
    s_upg.pack_file = UPGRADE_PACK_NET;
    pthread_mutex_unlock(&s_upg.lock);

    LOG_I("init ok");
    return 0;
}
