/**
 * @file    app_upgrade.c
 * @brief   OTA 固件升级业务完整实现
 *
 *    ① AppUpgradeHandleLongPack   → 快速写文件，不阻塞
 *    ② AppUpgradeHandleCtrlPack   → 收到执行命令后，在独立线程中
 *       先发应答，再执行升级
 *    ③ 升级线程中先关看门狗，再执行升级，保证顺序
 *    ④ 状态机（IDLE/RECEIVING/PROCESSING/DONE/ERROR）替代裸 static 变量
 */
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
 *  网络应答（调用 svc_network 发送控制包）
 * ========================================================= */
static void send_reply(uint8_t dst_dev, uint8_t arg1, uint8_t arg2)
{
    SvcNetworkUpgradeReply(dst_dev, arg1, arg2);
    printf("[AppUpgrade] → reply dev=0x%02X arg1=%d arg2=%d\n",
           dst_dev, arg1, arg2);
}

/* =========================================================
 *  Step1: 接收数据包，追加写入升级包文件
 *
 *  包序号从 1 开始，序号必须连续递增，否则判定为乱序失败。
 * ========================================================= */
static int recv_pack(uint32_t index, uint32_t len, const uint8_t *data)
{
    pthread_mutex_lock(&s_upg.lock);

    if (index == 1) {
        /* 第一包：清理旧文件，初始化计数 */
        s_upg.pkt_sum = 0;
        pthread_mutex_unlock(&s_upg.lock);

        printf("[AppUpgrade] *** RECEIVE START (ver %s %s) ***\n",
               __DATE__, __TIME__);
        system("rm -f "  UPGRADE_PACK_NET);
        system("touch "  UPGRADE_PACK_NET);
        system("chmod 777 " UPGRADE_PACK_NET);
        system("rm -rf " UPGRADE_TMP_PATH UPGRADE_RAR_DIR);

        pthread_mutex_lock(&s_upg.lock);
        s_upg.state   = UPGRADE_STATE_RECEIVING;
        s_upg.pkt_sum = 0;
        s_upg.pack_file = UPGRADE_PACK_NET;
    }

    int expected = s_upg.pkt_sum + 1;
    pthread_mutex_unlock(&s_upg.lock);

    if ((int)index != expected) {
        printf("[AppUpgrade] packet order error: expect=%d got=%u\n",
               expected, index);
        set_state(UPGRADE_STATE_ERROR);
        return 0;
    }

    /* 追加写入 */
    FILE *fp = fopen(UPGRADE_PACK_NET, "ab");
    if (!fp) {
        printf("[AppUpgrade] fopen fail\n");
        set_state(UPGRADE_STATE_ERROR);
        return 0;
    }
    int written = (int)fwrite(data, 1, (size_t)len, fp);
    fclose(fp);

    if ((uint32_t)written != len) {
        printf("[AppUpgrade] write error: want=%u got=%d\n", len, written);
        set_state(UPGRADE_STATE_ERROR);
        return 0;
    }

    pthread_mutex_lock(&s_upg.lock);
    s_upg.pkt_sum = (int)index;
    pthread_mutex_unlock(&s_upg.lock);

    printf("[AppUpgrade] pack %u ok (%u bytes, total pkts=%u)\n",
           index, len, index);
    return 1;
}

/* =========================================================
 *  Step2: 解压外层 .update 包，验证文件列表完整性
 *       
 * ========================================================= */
static const char *s_required_files[] = {
    UPGRADE_RAR_DIR, UPGRADE_PKG_RAR, UPGRADE_PKG_SHA1
};
#define REQUIRED_CNT  (int)(sizeof(s_required_files)/sizeof(s_required_files[0]))

static bool step1_decompress_check(const char *pack_path)
{
    printf("[AppUpgrade] --- Step1: decompress & verify file list ---\n");

    char cmd[512];
    /* 解压到 /tmp/ 并删除原包（节省空间）*/
    snprintf(cmd, sizeof(cmd),
             "tar -xzvf %s -C %s 2>&1; rm -f %s",
             pack_path, UPGRADE_TMP_PATH, pack_path);

    FILE *pf = popen(cmd, "r");
    if (!pf) { printf("[AppUpgrade] popen tar fail\n"); return false; }

    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), pf)) {
        printf("  tar: %s", line);
        for (int i = 0; i < REQUIRED_CNT; i++) {
            if (!(found & (1 << i)) && strstr(line, s_required_files[i]))
                found |= (1 << i);
        }
    }
    pclose(pf);

    int count = 0;
    for (int v = found; v; v &= v - 1) count++;
    printf("[AppUpgrade] found %d/%d required files (mask=0x%X)\n",
           count, REQUIRED_CNT, found);
    return (count == REQUIRED_CNT);
}

/* =========================================================
 *  Step3: 校验 image.tar.gz 的 SHA1
 *         
 * ========================================================= */
static bool step2_verify_sha1(void)
{
    printf("[AppUpgrade] --- Step2: verify SHA1 ---\n");

    char cmd[256], line[256];
    char current_hash[128] = {0};
    char correct_hash[256] = {0};

    /* 计算当前 sha1 */
    snprintf(cmd, sizeof(cmd), "sha1sum %s%s%s",
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR, UPGRADE_PKG_RAR);
    FILE *pf = popen(cmd, "r");
    if (!pf) { printf("[AppUpgrade] sha1sum fail\n"); return false; }
    if (fgets(line, sizeof(line), pf)) {
        char *sp = strchr(line, ' ');
        if (sp) {
            int n = (int)(sp - line);
            if (n > 127) n = 127;
            strncpy(current_hash, line, (size_t)n);
        }
    }
    pclose(pf);

    /* 读取期望 sha1 */
    char sha1_path[256];
    snprintf(sha1_path, sizeof(sha1_path), "%s%s%s",
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR, UPGRADE_PKG_SHA1);
    FILE *fp = fopen(sha1_path, "r");
    if (!fp) { printf("[AppUpgrade] open sha1 file fail\n"); return false; }
    fscanf(fp, "%255s", correct_hash);
    fclose(fp);

    bool ok = (strlen(correct_hash) > 0) &&
              (strncmp(correct_hash, current_hash, strlen(correct_hash)) == 0);
    printf("[AppUpgrade] SHA1 %s\n  current : %s\n  expected: %s\n",
           ok ? "PASS ✓" : "FAIL ✗", current_hash, correct_hash);
    return ok;
}

/* =========================================================
 *  Step4: 解压 image.tar.gz，执行 update.sh
 *      
 * ========================================================= */
static bool step3_flash(void)
{
    printf("[AppUpgrade] --- Step3: flash image ---\n");

    /* 解压 image.tar.gz 到 /tmp/，解压后删除压缩包和 image/ 目录 */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "tar -zxvf %s%s%s -C %s 2>&1; rm -rf %s%s",
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR, UPGRADE_PKG_RAR,
             UPGRADE_TMP_PATH,
             UPGRADE_TMP_PATH, UPGRADE_RAR_DIR);

    FILE *pf = popen(cmd, "r");
    if (!pf) { printf("[AppUpgrade] decompress image fail\n"); return false; }
    char line[256];
    while (fgets(line, sizeof(line), pf)) printf("  %s", line);
    pclose(pf);

    /* 验证 update.sh 存在 */
    if (access("/tmp/update.sh", F_OK) != 0) {
        printf("[AppUpgrade] /tmp/update.sh not found!\n"); return false;
    }

    printf("[AppUpgrade] *** Executing update.sh (no return) ***\n");
    fflush(stdout);

    /* 直接执行 update.sh（同步，update.sh 本身会重启系统）
     * 若 update.sh 执行失败会返回，此时 step3_flash 返回 false */
    int ret = system("/tmp/update.sh");
    if (ret != 0) {
        printf("[AppUpgrade] update.sh returned %d (should not happen)\n", ret);
        return false;
    }
    /* 正常情况下执行到这里说明系统即将重启，永远不会 return true */
    return true;
}

/* =========================================================
 *  升级主流程（在独立线程中执行，不阻塞事件回调）
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
    free(ta);

    printf("[AppUpgrade] *** UPGRADE THREAD START ***\n");
    set_state(UPGRADE_STATE_PROCESSING);

    /* ---- 先播放语音提示---- */
    SvcVoicePlaySimple(VOICE_Unlock16k, 90);

    /* ---- 等待 2s，让室内机收到应答后准备接收状态）----
     * 在独立线程中等待，不影响网络 */
    sleep(2);

    /* ---- 关闭看门狗（升级期间防止超时重启）---- */
    DrvWdtClose();
    printf("[AppUpgrade] watchdog disabled\n");

    /* ---- 三步升级流程 ---- */
    bool ok = step1_decompress_check(pack_path)
           && step2_verify_sha1()
           && step3_flash();

    if (!ok) {
        printf("[AppUpgrade] *** UPGRADE FAILED ***\n");
        set_state(UPGRADE_STATE_ERROR);

        /* 失败应答 */
        send_reply(sender, UPGRADE_REPLY_FAIL, 1);

        /* 清理临时文件 */
        system("rm -f " UPGRADE_PACK_NET);
        system("rm -rf " UPGRADE_TMP_PATH UPGRADE_RAR_DIR);
        system("rm -f /tmp/update.sh");

        /* 重新开启看门狗（恢复正常运行）*/
        DrvWdtOpen(10);
        DrvWdtFeed();

        /* 注意：EVT_UPGRADE_DONE 此处不需要发布。
         *   失败应答已由 send_reply(UPGRADE_REPLY_FAIL) 直接发给室内机。
         *   若业务层将来需要在门口机内部响应升级失败（如恢复灯光），
         *   在此处添加 EventBusPublish(EVT_UPGRADE_DONE, &result, sizeof(result))
         *   并在对应模块 EventBusSubscribe(EVT_UPGRADE_DONE, on_upgrade_done)。 */

        set_state(UPGRADE_STATE_IDLE);
    } else {
        /* step3_flash 调用 update.sh，update.sh 执行后系统重启，
         * 代码不会执行到此处，无需发布事件。 */
        set_state(UPGRADE_STATE_DONE);
    }

    printf("[AppUpgrade] upgrade thread exit\n");
    return NULL;
}

/* 启动升级线程（异步，不阻塞调用方）*/
static void start_upgrade_thread(uint8_t sender_dev, const char *pack_path)
{
    UpgradeThreadArg *ta = (UpgradeThreadArg *)malloc(sizeof(UpgradeThreadArg));
    if (!ta) { printf("[AppUpgrade] malloc fail\n"); return; }
    ta->sender_dev = sender_dev;
    strncpy(ta->pack_path, pack_path, sizeof(ta->pack_path) - 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, upgrade_thread, ta) != 0) {
        printf("[AppUpgrade] create upgrade thread fail\n");
        free(ta);
        return;
    }
    pthread_detach(tid);
    printf("[AppUpgrade] upgrade thread started\n");
}

/* =========================================================
 *  取消升级（清理临时文件）
 * ========================================================= */
static void cancel_upgrade(void)
{
    printf("[AppUpgrade] cancelled, cleaning temp files\n");
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

/**
 * @brief 处理长包（数据帧）
 *协议：
 *   int arg1 = DP[0]<<24|DP[1]<<16|DP[2]<<8|DP[3];  // 包序号
 *   int arg2 = DP[4]<<24|DP[5]<<16|DP[6]<<8|DP[7];  // 数据长度
 *   data = &DP[8]
 *   if (ReceiveUpgradePack(arg1, arg2, data) == 0) → 发 Arg1=3 失败应答
 */
int AppUpgradeHandleLongPack(uint8_t sender_dev,
                              uint32_t index, uint32_t data_len,
                              const uint8_t *data)
{
    /* 升级中或已完成时忽略新的数据包 */
    AppUpgradeState cur = get_state();
    if (cur == UPGRADE_STATE_PROCESSING || cur == UPGRADE_STATE_DONE) {
        printf("[AppUpgrade] in processing, ignore long pack\n");
        return 0;
    }

    printf("[AppUpgrade] long pack index=%u len=%u\n", index, data_len);

    if (recv_pack(index, data_len, data) == 0) {
        /* 收包失败 → 应答失败*/
        send_reply(sender_dev, UPGRADE_REPLY_FAIL, 1);
        return 0;
    }
    return 1;
}

/**
 * @brief 处理短包（控制帧）
 */
void AppUpgradeHandleCtrlPack(uint8_t sender_dev,
                               uint8_t ctrl_arg0, uint8_t ctrl_arg1)
{
    printf("[AppUpgrade] ctrl pack dev=0x%02X arg0=0x%02X arg1=0x%02X\n",
           sender_dev, ctrl_arg0, ctrl_arg1);

    /* ---- CheckOnlineStatus: Arg[0] & 0x01 ---- */
    if (ctrl_arg0 & 0x01) {
        /* 查询在线状态 → 立即应答 Arg1=1 Arg2=1 */
        send_reply(sender_dev, UPGRADE_REPLY_ONLINE, 1);
        return;
    }

    /* ---- UpdateOver: Arg[0] & 0x02 ---- */
    if (ctrl_arg0 & 0x02) {

        /* UpdataFinish: Arg[1] & 0x01 → 数据接收完毕，开始执行 */
        if (ctrl_arg1 & 0x01) {
            AppUpgradeState cur = get_state();
            if (cur == UPGRADE_STATE_RECEIVING || cur == UPGRADE_STATE_IDLE) {
                /* 检查升级包文件是否存在 */
                const char *pack_path = UPGRADE_PACK_NET;
                pthread_mutex_lock(&s_upg.lock);
                pack_path = s_upg.pack_file;
                pthread_mutex_unlock(&s_upg.lock);

                if (access(pack_path, F_OK) != 0) {
                    printf("[AppUpgrade] pack not found: %s\n", pack_path);
                    send_reply(sender_dev, UPGRADE_REPLY_FAIL, 1);
                    return;
                }

                /* 先发"开始执行"应答*/
                send_reply(sender_dev, UPGRADE_REPLY_EXECUTE, 1);

                /* 在独立线程中执行升级*/
                start_upgrade_thread(sender_dev, pack_path);

            } else {
                printf("[AppUpgrade] not in receiving state (%d), ignore\n", cur);
                send_reply(sender_dev, UPGRADE_REPLY_FAIL, 1);
            }
            return;
        }

        /* UpdataFail: Arg[1] & 0x02 → 取消*/
        if (ctrl_arg1 & 0x02) {
            cancel_upgrade();
            return;
        }
    }

    printf("[AppUpgrade] unknown ctrl pack arg0=0x%02X arg1=0x%02X\n",
           ctrl_arg0, ctrl_arg1);
}

/**
 * @brief SD 卡升级触发
 */
int AppUpgradeFromSD(void)
{
    if (get_state() != UPGRADE_STATE_IDLE) {
        printf("[AppUpgrade] already in progress\n"); return 0;
    }
    if (access(UPGRADE_PACK_SD, F_OK) != 0) {
        printf("[AppUpgrade] SD pack not found: %s\n", UPGRADE_PACK_SD);
        return 0;
    }

    printf("[AppUpgrade] SD card upgrade: %s\n", UPGRADE_PACK_SD);

    /* 拷贝到 /tmp 再执行（保持和网络升级相同的后续流程）*/
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cp %s %s", UPGRADE_PACK_SD, UPGRADE_PACK_NET);
    if (system(cmd) != 0) {
        printf("[AppUpgrade] cp SD pack fail\n"); return 0;
    }

    pthread_mutex_lock(&s_upg.lock);
    s_upg.pack_file = UPGRADE_PACK_NET;
    s_upg.state     = UPGRADE_STATE_RECEIVING;
    pthread_mutex_unlock(&s_upg.lock);

    /* SD 卡升级直接从 "执行" 阶段开始，sender_dev=0 表示本地触发 */
    send_reply(0, UPGRADE_REPLY_EXECUTE, 1);  /* 可选：无需应答直接执行 */
    start_upgrade_thread(0, UPGRADE_PACK_NET);
    return 1;
}

AppUpgradeState AppUpgradeGetState(void)
{
    return get_state();
}

int AppUpgradeInit(void)
{
    /* 清理可能残留的临时文件（异常断电恢复）*/
    if (access(UPGRADE_PACK_NET, F_OK) == 0) {
        printf("[AppUpgrade] stale net pack found, removing\n");
        system("rm -f " UPGRADE_PACK_NET);
    }
    system("rm -rf " UPGRADE_TMP_PATH UPGRADE_RAR_DIR);
    system("rm -f /tmp/update.sh");

    pthread_mutex_lock(&s_upg.lock);
    s_upg.state   = UPGRADE_STATE_IDLE;
    s_upg.pkt_sum = 0;
    s_upg.pack_file = UPGRADE_PACK_NET;
    pthread_mutex_unlock(&s_upg.lock);

    printf("[AppUpgrade] init ok\n");
    return 0;
}
