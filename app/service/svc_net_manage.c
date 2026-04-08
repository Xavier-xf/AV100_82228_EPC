/**
 * @file    svc_net_manage.c
 * @brief   网络卡片管理服务（TCP 4321 端口）
 *
 * 在独立线程中监听来自室内机 APP 的管理命令，对 IC 卡数据库进行增删查改。
 *
 * 协议格式：
 *   短包（8 字节）：[0xAA][dest][src][cmd][arg1][arg2][sum][0xCC]
 *   长包（1288 字节）：[0xBB][dest][src][cmd][arg][0x00][data 1280B][0xCC]
 */
#define LOG_TAG "NetMgr"
#include "log.h"

#include "svc_net_manage.h"
#include "app_card.h"
#include "svc_timer.h"
#include "svc_voice.h"
#include "drv_gpio.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

/* =========================================================
 *  协议常量
 * ========================================================= */
#define NET_MANAGE_PORT    4321
#define SHORT_PACK_LEN     8      /* 短包固定长度（字节）           */
#define LONG_PACK_LEN      1288   /* 长包固定长度（字节）           */
#define SHORT_PACK_START   0xAA   /* 短包起始字节                   */
#define LONG_PACK_START    0xBB   /* 长包起始字节                   */
#define PACK_END           0xCC   /* 包结束字节                     */

/* =========================================================
 *  内部状态
 * ========================================================= */
static int             s_server_fd   = -1;  /* 监听 socket           */
static int             s_client_fd   = -1;  /* 当前连接的客户端 fd   */
static int             s_connected   = 0;   /* 是否有客户端在线       */
static pthread_mutex_t s_send_lock   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_state_lock  = PTHREAD_MUTEX_INITIALIZER;

/* =========================================================
 *  TCP 工具
 * ========================================================= */
static int tcp_server_init(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[NetMgr] socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[NetMgr] bind"); close(fd); return -1;
    }
    if (listen(fd, 1) < 0) {
        perror("[NetMgr] listen"); close(fd); return -1;
    }
    return fd;
}

/* 非阻塞 accept，超时 50ms，成功返回客户端 fd，无连接返回 0 */
static int tcp_accept(int server_fd)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(server_fd, &rd);
    struct timeval tv = {0, 50000};
    int ret = select(server_fd + 1, &rd, NULL, NULL, &tv);
    if (ret > 0) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int cfd = accept(server_fd, (struct sockaddr *)&cli, &len);
        if (cfd >= 0)
            LOG_I("client connected: %s", inet_ntoa(cli.sin_addr));
        return cfd;
    }
    return 0;
}

/**
 * 带超时的 recv。
 * @return >0  收到字节数
 *          0  连接正常关闭
 *         -1  超时（无数据，非致命）
 *         -2  recv 出错（连接异常）
 */
static int tcp_recv(int fd, void *buf, int size, int timeout_ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int ret = select(fd + 1, &rd, NULL, NULL, &tv);
    if (ret > 0) {
        int n = (int)recv(fd, buf, (size_t)size, MSG_DONTWAIT);
        if (n == 0)  return 0;
        if (n < 0)   return -2;
        return n;
    }
    return -1;
}

static int tcp_send(int fd, const unsigned char *buf, int len)
{
    pthread_mutex_lock(&s_send_lock);
    int n = (int)send(fd, buf, (size_t)len, 0);
    pthread_mutex_unlock(&s_send_lock);
    return n;
}

/* =========================================================
 *  发包接口
 * ========================================================= */

/**
 * @brief 发送短包（8 字节）
 *
 * 短包布局：
 *   [0] 0xAA  起始
 *   [1] 0x01  目标设备（室内机）
 *   [2] 0x01  源设备（本机）
 *   [3] cmd   命令字
 *   [4] arg1  参数1
 *   [5] arg2  参数2
 *   [6] sum   校验（[1]+[2]+[3]+[4]+[5]）& 0xFF
 *   [7] 0xCC  结束
 */
void SvcNetManageSendShort(unsigned char cmd,
                           unsigned char arg1,
                           unsigned char arg2)
{
    int cfd = s_client_fd;
    if (cfd < 0) return;

    unsigned char pkt[SHORT_PACK_LEN];
    pkt[0] = SHORT_PACK_START;
    pkt[1] = 1;
    pkt[2] = 1;
    pkt[3] = cmd;
    pkt[4] = arg1;
    pkt[5] = arg2;
    pkt[6] = (unsigned char)((pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5]) & 0xFF);
    pkt[7] = PACK_END;

    if (tcp_send(cfd, pkt, SHORT_PACK_LEN) <= 0)
        LOG_E("send short cmd=0x%02X fail", cmd);
    else
        LOG_D("send short cmd=0x%02X ok", cmd);
}

/**
 * @brief 发送长包（1288 字节）
 *
 * 长包布局：
 *   [0]      0xBB       起始
 *   [1]      0x01       目标设备（室内机）
 *   [2]      0x01       源设备（本机）
 *   [3]      cmd        命令字
 *   [4]      arg        参数
 *   [5]      0x00       保留
 *   [6..1286] data      有效数据（最多 1281 字节）
 *   [1287]   0xCC       结束
 */
static void send_long(unsigned char cmd, unsigned char arg,
                      unsigned char *data, int data_len)
{
    int cfd = s_client_fd;
    if (cfd < 0) return;

    unsigned char pkt[LONG_PACK_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = LONG_PACK_START;
    pkt[1] = 1;
    pkt[2] = 1;
    pkt[3] = cmd;
    pkt[4] = arg;
    /* pkt[5] 保留，保持 0x00 */
    int copy_len = data_len < (LONG_PACK_LEN - 7) ? data_len : (LONG_PACK_LEN - 7);
    memcpy(&pkt[6], data, (size_t)copy_len);
    pkt[LONG_PACK_LEN - 1] = PACK_END;

    if (tcp_send(cfd, pkt, LONG_PACK_LEN) <= 0)
        LOG_E("send long cmd=0x%02X fail", cmd);
    else
        LOG_D("send long cmd=0x%02X ok", cmd);
}

/* =========================================================
 *  包校验
 * ========================================================= */
typedef enum { PACK_INVALID = 0, PACK_SHORT, PACK_LONG } PackType;

static PackType check_packet(const unsigned char *buf, int len)
{
    if (len <= 0) return PACK_INVALID;
    if (buf[2] != 1 && buf[2] != 0xFF) return PACK_INVALID;  /* 源设备必须是室内机 1 或广播 */
    if (buf[len - 1] != PACK_END)      return PACK_INVALID;
    if (buf[0] == SHORT_PACK_START && len == SHORT_PACK_LEN) return PACK_SHORT;
    if (buf[0] == LONG_PACK_START  && len == LONG_PACK_LEN)  return PACK_LONG;
    return PACK_INVALID;
}

/* =========================================================
 *  添加卡模式指示灯闪烁
 * ========================================================= */
static void manage_light_cb(void *arg)
{
    (void)arg;
    if (SvcTimerActive(TMR_ADD_CARD)) {
        static int light_on = 1;
        DrvGpioCardLightSet((light_on = !light_on));
        SvcTimerSet(TMR_MANAGE_LIGHT, 200, manage_light_cb, NULL);
    } else {
        DrvGpioCardLightSet(0);
    }
}

/* =========================================================
 *  命令处理
 * ========================================================= */
static void handle_packet(PackType type, const unsigned char *buf, int len)
{
    (void)len;
    unsigned char cmd  = buf[3];
    unsigned char arg1 = buf[4];
    unsigned char arg2 = buf[5];

    LOG_D("cmd=0x%02X arg1=%d arg2=%d", cmd, arg1, arg2);

    switch (cmd) {

    /* 进入添加卡模式：30s 超时，指示灯开始闪烁 */
    case NET_MGR_ADD_CARD:
        SvcTimerSet(TMR_ADD_CARD, 30000, NULL, NULL);
        SvcTimerSet(TMR_MANAGE_LIGHT, 200, manage_light_cb, NULL);
        LOG_I("enter add-card mode");
        break;

    /* 删除卡片：arg1 == DECK_SIZE_MAX 表示格式化全部；否则删除指定索引 */
    case NET_MGR_DEL_CARD:
        if (arg1 == DECK_SIZE_MAX) {
            AppCardDeckFormat();
            LOG_I("deck format all");
        } else if (arg1 < DECK_SIZE_MAX) {
            AppCardSetPerm(arg1, 0);
            LOG_I("del card[%d]", arg1);
        }
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        break;

    /* 校验卡片（预留接口，暂无操作）*/
    case NET_MGR_VERIFY_CARD:
        break;

    /* 获取卡组权限列表，以长包形式回传 */
    case NET_MGR_GET_CARD: {
        unsigned char *deck_buf;
        int deck_len = AppCardDeckPermGet(&deck_buf);
        LOG_I("send deck len=%d", deck_len);
        send_long(NET_MGR_GET_CARD, 0, deck_buf, deck_len);
        break;
    }

    /* 设置指定卡索引的权限位 */
    case NET_MGR_SET_CARD_PERM:
        AppCardSetPerm(arg1, arg2);
        LOG_I("set perm card[%d]=%d", arg1, arg2);
        break;

    /* 退出添加卡模式，关闭指示灯并通知室内机 */
    case NET_MGR_EXIT_CARD:
        SvcTimerStop(TMR_ADD_CARD);
        DrvGpioCardLightSet(0);
        SvcNetManageSendShort(NET_MGR_EXIT_CARD, 0, 0);
        SvcVoicePlaySimple(VOICE_Bi4, VOICE_VOL_DEFAULT);
        LOG_I("exit add-card mode");
        break;

    /* 拒绝访问：回复 arg2=1 通知室内机 */
    case NET_MGR_ACCESS_DENIED:
        SvcNetManageSendShort(NET_MGR_ACCESS_DENIED, arg1, 1);
        LOG_W("access denied, notify indoor");
        break;

    default:
        LOG_W("unknown cmd=0x%02X", cmd);
        break;
    }

    (void)type;
    (void)arg2;
}

/* =========================================================
 *  服务线程
 * ========================================================= */
static void *net_manage_thread(void *arg)
{
    (void)arg;
    unsigned char buf[LONG_PACK_LEN];

    while (1) {
        int cfd = tcp_accept(s_server_fd);
        if (cfd <= 0) continue;

        s_client_fd = cfd;
        pthread_mutex_lock(&s_state_lock);
        s_connected = 1;
        pthread_mutex_unlock(&s_state_lock);
        LOG_I("session start fd=%d", cfd);

        while (1) {
            memset(buf, 0, sizeof(buf));
            int n = tcp_recv(cfd, buf, sizeof(buf), 5);
            if (n == 0 || n == -2) break;
            if (n < 0) { usleep(1000); continue; }

            PackType pt = check_packet(buf, n);
            if (pt != PACK_INVALID)
                handle_packet(pt, buf, n);
            usleep(1000);
        }

        close(cfd);
        s_client_fd = -1;
        pthread_mutex_lock(&s_state_lock);
        s_connected = 0;
        pthread_mutex_unlock(&s_state_lock);
        SvcTimerStop(TMR_ADD_CARD);
        DrvGpioCardLightSet(0);
        LOG_I("session end fd=%d", cfd);
    }
    return NULL;
}

/* =========================================================
 *  公共接口
 * ========================================================= */
int SvcNetManageConnected(void)
{
    pthread_mutex_lock(&s_state_lock);
    int v = s_connected;
    pthread_mutex_unlock(&s_state_lock);
    return v;
}

int SvcNetManageInit(void)
{
    s_server_fd = tcp_server_init(NET_MANAGE_PORT);
    if (s_server_fd < 0) {
        LOG_E("server init fail on port %d", NET_MANAGE_PORT);
        return -1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, net_manage_thread, NULL) != 0) {
        LOG_E("create thread fail");
        close(s_server_fd);
        s_server_fd = -1;
        return -1;
    }
    pthread_detach(tid);
    LOG_I("server ready on port %d", NET_MANAGE_PORT);
    return 0;
}
