/**
 * @file    svc_net_manage.c
 * @brief   网络卡片管理服务（TCP 4321 端口）
 *
 * 移植自旧版 UserNetManage.c + NetManageCom.c。
 * 在独立线程中监听来自室内机 APP 的管理命令，对 IC 卡数据库进行操作。
 */
#include "svc_net_manage.h"
#include "app_card.h"
#include "svc_timer.h"
#include "svc_voice.h"
#include "drv_gpio.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdatomic.h>
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
#define SHORT_PACK_LEN     8
#define LONG_PACK_LEN      1288
#define SHORT_PACK_START   0xAA
#define LONG_PACK_START    0xBB
#define PACK_END           0xCC

/* =========================================================
 *  内部状态
 * ========================================================= */
static int           s_server_fd  = -1;
static int           s_client_fd  = -1;
static atomic_int    s_connected  = ATOMIC_VAR_INIT(0);
static pthread_mutex_t s_send_lock = PTHREAD_MUTEX_INITIALIZER;

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

static int tcp_accept(int server_fd)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(server_fd, &rd);
    struct timeval tv = {0, 50000};  /* 50ms */
    int ret = select(server_fd + 1, &rd, NULL, NULL, &tv);
    if (ret > 0) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int cfd = accept(server_fd, (struct sockaddr *)&cli, &len);
        if (cfd >= 0)
            printf("[NetMgr] client connected: %s\n", inet_ntoa(cli.sin_addr));
        return cfd;
    }
    return 0;
}

/* 返回值：
 *  > 0  : 接收到的字节数
 *   0   : 连接已断开（recv 返回 0）
 *  -1   : 超时（select 无数据），调用方继续等待
 *  -2   : recv 出错（连接异常）
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
        if (n == 0)  return 0;   /* 连接正常关闭 */
        if (n < 0)   return -2;  /* recv 出错     */
        return n;
    }
    return -1;  /* 超时，非致命 */
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

/** 发送短包（8 字节）*/
void SvcNetManageSendShort(unsigned char cmd,
                           unsigned char arg1,
                           unsigned char arg2)
{
    int cfd = s_client_fd;
    if (cfd < 0) return;

    unsigned char pkt[SHORT_PACK_LEN];
    pkt[0] = SHORT_PACK_START;
    pkt[1] = 1;          /* 目标设备（室内机 = 1）*/
    pkt[2] = 1;          /* 源设备（本地，简化）  */
    pkt[3] = cmd;
    pkt[4] = arg1;
    pkt[5] = arg2;
    pkt[6] = (unsigned char)((pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5]) & 0xFF);
    pkt[7] = PACK_END;
    if (tcp_send(cfd, pkt, SHORT_PACK_LEN) <= 0)
        printf("[NetMgr] send short cmd=0x%02X fail\n", cmd);
    else
        printf("[NetMgr] send short cmd=0x%02X ok\n", cmd);
}

/** 发送长包（1288 字节，与旧版 NetManageLongPack 格式相同）
 *  布局：[0xBB][dest][src][cmd][arg][0x00][data 1280B][0xCC]
 */
static void send_long(unsigned char cmd, unsigned char arg,
                      unsigned char *data, int data_len)
{
    int cfd = s_client_fd;
    if (cfd < 0) return;

    unsigned char pkt[LONG_PACK_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = LONG_PACK_START;   /* 0xBB */
    pkt[1] = 1;                 /* dest=室内机 */
    pkt[2] = 1;                 /* src=本机    */
    pkt[3] = cmd;
    pkt[4] = arg;
    /* pkt[5] 留空（与旧版 Code[5]=0 一致）*/
    int copy_len = data_len < (LONG_PACK_LEN - 7) ? data_len : (LONG_PACK_LEN - 7);
    memcpy(&pkt[6], data, (size_t)copy_len);   /* 数据从 byte 6 开始 */
    pkt[LONG_PACK_LEN - 1] = PACK_END;         /* 0xCC */
    if (tcp_send(cfd, pkt, LONG_PACK_LEN) <= 0)
        printf("[NetMgr] send long cmd=0x%02X fail\n", cmd);
    else
        printf("[NetMgr] send long cmd=0x%02X ok\n", cmd);
}

/* =========================================================
 *  包校验
 * ========================================================= */
typedef enum { PACK_INVALID = 0, PACK_SHORT, PACK_LONG } PackType;

static PackType check_packet(const unsigned char *buf, int len)
{
    if (len <= 0) return PACK_INVALID;
    /* 源设备必须是 1 或广播 0xFF */
    if (buf[2] != 1 && buf[2] != 0xFF) return PACK_INVALID;
    if (buf[len - 1] != PACK_END)     return PACK_INVALID;
    if (buf[0] == SHORT_PACK_START && len == SHORT_PACK_LEN) return PACK_SHORT;
    if (buf[0] == LONG_PACK_START  && len == LONG_PACK_LEN)  return PACK_LONG;
    return PACK_INVALID;
}

/* =========================================================
 *  管理灯闪烁（添加卡模式指示）
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

    printf("[NetMgr] cmd=0x%02X arg1=%d arg2=%d\n", cmd, arg1, arg2);

    switch (cmd) {
    /* ---- 进入添加卡模式 ---- */
    case NET_MGR_ADD_CARD:
        SvcTimerSet(TMR_ADD_CARD, 30000, NULL, NULL);
        SvcTimerSet(TMR_MANAGE_LIGHT, 200, manage_light_cb, NULL);
        break;

    /* ---- 删除卡片 ---- */
    case NET_MGR_DEL_CARD:
        if (arg1 == DECK_SIZE_MAX) {
            AppCardDeckFormat();
        } else if (arg1 < DECK_SIZE_MAX) {
            AppCardSetPerm(arg1, 0);
        }
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        break;

    /* ---- 校验卡片（预留，无操作）---- */
    case NET_MGR_VERIFY_CARD:
        break;

    /* ---- 获取卡组列表 ---- */
    case NET_MGR_GET_CARD: {
        unsigned char *deck_buf;
        int deck_len = AppCardDeckPermGet(&deck_buf);
        printf("[NetMgr] send deck, len=%d\n", deck_len);
        send_long(NET_MGR_GET_CARD, 0, deck_buf, deck_len);
        break;
    }

    /* ---- 设置卡片权限 ---- */
    case NET_MGR_SET_CARD_PERM:
        AppCardSetPerm(arg1, arg2);
        break;

    /* ---- 退出添加卡模式 ---- */
    case NET_MGR_EXIT_CARD:
        SvcTimerStop(TMR_ADD_CARD);
        DrvGpioCardLightSet(0);
        SvcNetManageSendShort(NET_MGR_EXIT_CARD, 0, 0);
        SvcVoicePlaySimple(VOICE_Bi4, VOICE_VOL_DEFAULT);
        break;

    /* ---- 拒绝访问 ---- */
    case NET_MGR_ACCESS_DENIED:
        SvcNetManageSendShort(NET_MGR_ACCESS_DENIED, arg1, 1);
        break;

    default:
        printf("[NetMgr] unknown cmd=0x%02X\n", cmd);
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
        /* 等待客户端连接 */
        int cfd = tcp_accept(s_server_fd);
        if (cfd <= 0) continue;

        s_client_fd = cfd;
        atomic_store(&s_connected, 1);
        printf("[NetMgr] session start, fd=%d\n", cfd);

        while (1) {
            memset(buf, 0, sizeof(buf));
            int n = tcp_recv(cfd, buf, sizeof(buf), 5);  /* 5ms 轮询 */
            if (n == 0 || n == -2) break;  /* 连接断开或出错 */
            if (n < 0) {
                usleep(1000);  /* 超时，继续等待 */
                continue;
            }
            PackType pt = check_packet(buf, n);
            if (pt != PACK_INVALID)
                handle_packet(pt, buf, n);
            usleep(1000);
        }

        close(cfd);
        s_client_fd = -1;
        atomic_store(&s_connected, 0);
        /* 退出添加卡模式 */
        SvcTimerStop(TMR_ADD_CARD);
        DrvGpioCardLightSet(0);
        printf("[NetMgr] session end, fd=%d\n", cfd);
    }
    return NULL;
}

/* =========================================================
 *  公共接口
 * ========================================================= */
int SvcNetManageConnected(void)
{
    return atomic_load(&s_connected);
}

int SvcNetManageInit(void)
{
    s_server_fd = tcp_server_init(NET_MANAGE_PORT);
    if (s_server_fd < 0) {
        printf("[NetMgr] server init fail on port %d\n", NET_MANAGE_PORT);
        return -1;
    }
    printf("[NetMgr] server ready on port %d\n", NET_MANAGE_PORT);

    pthread_t tid;
    if (pthread_create(&tid, NULL, net_manage_thread, NULL) != 0) {
        printf("[NetMgr] create thread fail\n");
        close(s_server_fd);
        s_server_fd = -1;
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
