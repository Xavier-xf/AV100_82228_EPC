/**
 * @file    svc_net_manage.c
 * @brief   网络卡片管理服务（TCP 4321 端口）
 *
 * 在独立线程中监听来自室内机 APP 的管理命令，对 IC 卡数据库进行增删查改。
 *
 * 协议格式：
 *   短包（ 8 字节）：[0xAA][dest][src][cmd][arg1][arg2][sum][0xCC]
 *   长包（1288字节）：[0xBB][dest][src][cmd][arg ][0x00][data 1281B][0xCC]
 *
 * 实现要点：
 *   - TCP 是字节流，用 RxFramer 按起始字节定长组帧，避免粘包/拆包错位。
 *   - 短包 sum 字段必须校验，防止通信抖动误触发"格式化卡库"等高危命令。
 *   - send 使用 MSG_NOSIGNAL + 循环发送 + 总超时，消除 SIGPIPE 和部分写隐患。
 *   - 会话层有空闲超时 + TCP keepalive 双重保险，防止死连接阻塞后续 accept。
 *   - 所有运行期状态收敛到 SvcNetMgrCtx / RxFramer / LightBlinkCtx 三个结构体。
 */
#define LOG_TAG "NetMgr"
#include "log.h"

#include "svc_net_manage.h"
#include "app_card.h"
#include "svc_timer.h"
#include "svc_voice.h"
#include "drv_gpio.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

/* =========================================================
 *  协议常量
 * ========================================================= */
#define NET_MANAGE_PORT        4321

#define SHORT_PACK_LEN         8
#define LONG_PACK_LEN          1288
#define SHORT_PACK_START       0xAA
#define LONG_PACK_START        0xBB
#define PACK_END               0xCC

/* 协议字段偏移（短/长包前 6 字节布局一致）*/
#define PKT_OFF_START          0
#define PKT_OFF_DEST           1
#define PKT_OFF_SRC            2
#define PKT_OFF_CMD            3
#define PKT_OFF_ARG1           4
#define PKT_OFF_ARG2           5  /* 短包=arg2；长包=保留 */
#define SHORT_OFF_SUM          6

/* 长包有效载荷布局 */
#define LONG_PACK_HEADER_LEN   6
#define LONG_PACK_TAIL_LEN     1
#define LONG_PACK_DATA_MAX     (LONG_PACK_LEN - LONG_PACK_HEADER_LEN - LONG_PACK_TAIL_LEN) /* 1281 */
#define LONG_OFF_DATA          LONG_PACK_HEADER_LEN

/* 设备地址 */
#define DEVICE_ID_SELF         0x01
#define DEVICE_ID_INDOOR       0x01
#define DEVICE_ID_BROADCAST    0xFF

/* 时序参数 */
#define ACCEPT_TIMEOUT_MS       100   /* accept 单次 select 超时 */
#define RECV_TIMEOUT_MS         100   /* 每次 read 单次 select 超时 */
#define SEND_TOTAL_TIMEOUT_MS   2000  /* 单次 send_all 总超时 */
#define SESSION_IDLE_TIMEOUT_MS 60000 /* 60s 无数据则断开连接 */
#define KEEPALIVE_IDLE_SEC      10    /* TCP keepalive 参数 */
#define KEEPALIVE_INTVL_SEC     5
#define KEEPALIVE_CNT           3
#define LIGHT_BLINK_PERIOD_MS   200
#define ADD_CARD_TIMEOUT_MS     30000

/* =========================================================
 *  运行期状态（全部收敛到结构体）
 * ========================================================= */

/** 服务上下文：socket、客户端状态、串行化锁 */
typedef struct {
    pthread_mutex_t lock;         /* 保护 client_fd / connected 的原子读写 */
    pthread_mutex_t send_lock;    /* 串行化并发 send */
    int             server_fd;
    int             client_fd;    /* -1 表示无连接 */
    int             connected;
} SvcNetMgrCtx;

static SvcNetMgrCtx s_ctx = {
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .send_lock = PTHREAD_MUTEX_INITIALIZER,
    .server_fd = -1,
    .client_fd = -1,
    .connected = 0,
};

/** 接收端组帧器：按起始字节定长读取，解决 TCP 粘包/拆包 */
typedef struct {
    unsigned char buf[LONG_PACK_LEN];
    int           have;   /* 当前缓存字节数 */
    int           need;   /* 已识别到的期望包长度，0 表示尚未确定 */
} RxFramer;

/** 添加卡模式指示灯闪烁状态 */
typedef struct {
    int running;
    int on;
} LightBlinkCtx;

static LightBlinkCtx s_blink = { 0, 0 };

/* =========================================================
 *  状态访问小工具（消除裸静态变量的多线程读写）
 * ========================================================= */
static int ctx_get_client_fd(void)
{
    pthread_mutex_lock(&s_ctx.lock);
    int fd = s_ctx.client_fd;
    pthread_mutex_unlock(&s_ctx.lock);
    return fd;
}

static void ctx_set_session(int fd, int connected)
{
    pthread_mutex_lock(&s_ctx.lock);
    s_ctx.client_fd = fd;
    s_ctx.connected = connected;
    pthread_mutex_unlock(&s_ctx.lock);
}

static long ts_elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec  - start->tv_sec)  * 1000L
         + (long)(now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* =========================================================
 *  TCP 底层工具
 * ========================================================= */

/** 创建监听 socket。失败返回 -1。 */
static int tcp_server_init(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG_E("socket: %s", strerror(errno)); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("bind %d: %s", port, strerror(errno)); close(fd); return -1;
    }
    if (listen(fd, 1) < 0) {
        LOG_E("listen: %s", strerror(errno)); close(fd); return -1;
    }
    return fd;
}

/** 给已连接 socket 设置 TCP keepalive，死连接检测约 25s 可感知。 */
static void tcp_enable_keepalive(int fd)
{
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0)
        LOG_W("SO_KEEPALIVE: %s", strerror(errno));
#ifdef TCP_KEEPIDLE
    int v = KEEPALIVE_IDLE_SEC;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &v, sizeof(v));
#endif
#ifdef TCP_KEEPINTVL
    int i = KEEPALIVE_INTVL_SEC;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &i, sizeof(i));
#endif
#ifdef TCP_KEEPCNT
    int c = KEEPALIVE_CNT;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &c, sizeof(c));
#endif
}

/**
 * 非阻塞 accept。
 * @return >0: 客户端 fd； 0: 超时无连接； -1: accept 错误
 */
static int tcp_accept(int server_fd, int timeout_ms)
{
    fd_set rd; FD_ZERO(&rd); FD_SET(server_fd, &rd);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int sel = select(server_fd + 1, &rd, NULL, NULL, &tv);
    if (sel <= 0) return 0;

    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);
    int cfd = accept(server_fd, (struct sockaddr *)&cli, &len);
    if (cfd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            LOG_W("accept: %s", strerror(errno));
        return -1;
    }
    tcp_enable_keepalive(cfd);
    LOG_I("client connected: %s", inet_ntoa(cli.sin_addr));
    return cfd;
}

/**
 * 单次读取到缓冲。
 * @return  >0: 读到字节数； 0: 对端正常关闭； -1: 本次超时； -2: 出错
 */
static int tcp_read_once(int fd, void *buf, int size, int timeout_ms)
{
    fd_set rd; FD_ZERO(&rd); FD_SET(fd, &rd);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int sel = select(fd + 1, &rd, NULL, NULL, &tv);
    if (sel == 0)  return -1;
    if (sel < 0)   return (errno == EINTR) ? -1 : -2;

    ssize_t n = recv(fd, buf, (size_t)size, MSG_DONTWAIT);
    if (n == 0) return 0;
    if (n  < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    return (int)n;
}

/**
 * 循环发送直到全部写完或超时。屏蔽 SIGPIPE。
 * @return  >=0: 已发送字节数（== len 才算成功）；-1: 超时； -2: 出错
 */
static int tcp_send_all(int fd, const unsigned char *buf, int len, int total_timeout_ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int sent = 0;
    while (sent < len) {
        long elapsed = ts_elapsed_ms(&start);
        if (elapsed >= total_timeout_ms) return -1;
        long remain = total_timeout_ms - elapsed;

        fd_set wr; FD_ZERO(&wr); FD_SET(fd, &wr);
        struct timeval tv = { remain / 1000, (remain % 1000) * 1000 };
        int sel = select(fd + 1, NULL, &wr, NULL, &tv);
        if (sel == 0) return -1;
        if (sel < 0)  { if (errno == EINTR) continue; return -2; }

        ssize_t n = send(fd, buf + sent, (size_t)(len - sent),
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) { sent += (int)n; continue; }
        if (n == 0) return -2;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        return -2;
    }
    return sent;
}

/** 对当前客户端发送一段字节。成功返回 0，失败 -1。 */
static int net_send_bytes(const unsigned char *buf, int len)
{
    int fd = ctx_get_client_fd();
    if (fd < 0) return -1;

    pthread_mutex_lock(&s_ctx.send_lock);
    int r = tcp_send_all(fd, buf, len, SEND_TOTAL_TIMEOUT_MS);
    pthread_mutex_unlock(&s_ctx.send_lock);

    if (r != len) {
        LOG_E("send_all fail: r=%d want=%d errno=%s", r, len, strerror(errno));
        return -1;
    }
    return 0;
}

/* =========================================================
 *  包构造 / 校验
 * ========================================================= */

/** 计算短包 sum（pkt[1..5] 求和取低 8 位）。对短/长包共用前 6 字节段。 */
static unsigned char short_checksum(const unsigned char *pkt)
{
    unsigned int s = (unsigned int)pkt[PKT_OFF_DEST]
                   + (unsigned int)pkt[PKT_OFF_SRC]
                   + (unsigned int)pkt[PKT_OFF_CMD]
                   + (unsigned int)pkt[PKT_OFF_ARG1]
                   + (unsigned int)pkt[PKT_OFF_ARG2];
    return (unsigned char)(s & 0xFF);
}

typedef enum { PACK_INVALID = 0, PACK_SHORT, PACK_LONG } PackType;

/** 校验已组帧的完整包，返回类型。 */
static PackType check_packet(const unsigned char *buf, int len)
{
    if (len < SHORT_PACK_LEN) return PACK_INVALID;

    /* 源设备必须是室内机或广播 */
    unsigned char src = buf[PKT_OFF_SRC];
    if (src != DEVICE_ID_INDOOR && src != DEVICE_ID_BROADCAST) return PACK_INVALID;

    /* 目标设备必须是本机或广播 */
    unsigned char dest = buf[PKT_OFF_DEST];
    if (dest != DEVICE_ID_SELF && dest != DEVICE_ID_BROADCAST) return PACK_INVALID;

    if (buf[len - 1] != PACK_END) return PACK_INVALID;

    if (buf[0] == SHORT_PACK_START && len == SHORT_PACK_LEN) {
        if (buf[SHORT_OFF_SUM] != short_checksum(buf)) {
            LOG_W("short checksum mismatch: got=0x%02X want=0x%02X",
                  buf[SHORT_OFF_SUM], short_checksum(buf));
            return PACK_INVALID;
        }
        return PACK_SHORT;
    }
    if (buf[0] == LONG_PACK_START && len == LONG_PACK_LEN)
        return PACK_LONG;

    return PACK_INVALID;
}

/* =========================================================
 *  发包接口
 * ========================================================= */
void SvcNetManageSendShort(unsigned char cmd,
                           unsigned char arg1,
                           unsigned char arg2)
{
    unsigned char pkt[SHORT_PACK_LEN];
    pkt[PKT_OFF_START] = SHORT_PACK_START;
    pkt[PKT_OFF_DEST]  = DEVICE_ID_INDOOR;
    pkt[PKT_OFF_SRC]   = DEVICE_ID_SELF;
    pkt[PKT_OFF_CMD]   = cmd;
    pkt[PKT_OFF_ARG1]  = arg1;
    pkt[PKT_OFF_ARG2]  = arg2;
    pkt[SHORT_OFF_SUM] = short_checksum(pkt);
    pkt[SHORT_PACK_LEN - 1] = PACK_END;

    if (net_send_bytes(pkt, SHORT_PACK_LEN) < 0)
        LOG_E("send short cmd=0x%02X fail", cmd);
    else
        LOG_D("send short cmd=0x%02X ok", cmd);
}

static void send_long(unsigned char cmd, unsigned char arg,
                      const unsigned char *data, int data_len)
{
    if (data_len < 0) data_len = 0;
    if (data_len > LONG_PACK_DATA_MAX) {
        LOG_E("send long cmd=0x%02X: data too large (%d > %d)",
              cmd, data_len, LONG_PACK_DATA_MAX);
        return;
    }

    unsigned char pkt[LONG_PACK_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[PKT_OFF_START] = LONG_PACK_START;
    pkt[PKT_OFF_DEST]  = DEVICE_ID_INDOOR;
    pkt[PKT_OFF_SRC]   = DEVICE_ID_SELF;
    pkt[PKT_OFF_CMD]   = cmd;
    pkt[PKT_OFF_ARG1]  = arg;
    /* pkt[PKT_OFF_ARG2] 保留 0x00 */
    if (data_len > 0 && data != NULL)
        memcpy(&pkt[LONG_OFF_DATA], data, (size_t)data_len);
    pkt[LONG_PACK_LEN - 1] = PACK_END;

    if (net_send_bytes(pkt, LONG_PACK_LEN) < 0)
        LOG_E("send long cmd=0x%02X fail", cmd);
    else
        LOG_D("send long cmd=0x%02X ok (data=%d)", cmd, data_len);
}

/* =========================================================
 *  接收端组帧（解决 TCP 粘包/拆包）
 *
 *  规则：
 *   1. 缓存首字节必须是 0xAA 或 0xBB，否则丢弃 1 字节重扫。
 *   2. 首字节确定 need（8 或 1288），等到攒够 need 字节后整体交 check_packet。
 *   3. check_packet 失败 → resync（丢弃 1 字节，重新找起始）。
 *   4. 成功 → consume（移除 need 字节）。
 * ========================================================= */
static void framer_reset(RxFramer *f)
{
    f->have = 0;
    f->need = 0;
}

static int framer_free_space(const RxFramer *f)
{
    return LONG_PACK_LEN - f->have;
}

/** 把已读到的字节追加到组帧器缓冲。 */
static void framer_append(RxFramer *f, const unsigned char *src, int n)
{
    if (n <= 0) return;
    int space = framer_free_space(f);
    if (n > space) n = space;
    memcpy(f->buf + f->have, src, (size_t)n);
    f->have += n;
}

/** 丢弃缓冲头部 n 字节（n=1 用于 resync；n=need 用于 consume）。 */
static void framer_drop_front(RxFramer *f, int n)
{
    if (n <= 0)        return;
    if (n >= f->have)  { f->have = 0; f->need = 0; return; }
    memmove(f->buf, f->buf + n, (size_t)(f->have - n));
    f->have -= n;
    f->need = 0;
}

/**
 * 尝试从缓存中解析出一个完整包。
 * @return 1=已解析（[0..*out_len-1] 就是完整包，待 consume）
 *         0=还需更多数据
 *        -1=当前起始字节无效（已自动丢 1 字节重扫，请再次调用）
 */
static int framer_try_parse(RxFramer *f, int *out_len)
{
    if (f->have == 0) return 0;

    /* 尚未锁定起始：首字节必须是 0xAA/0xBB，否则丢 1 字节重扫 */
    if (f->need == 0) {
        unsigned char s = f->buf[0];
        if (s == SHORT_PACK_START)      f->need = SHORT_PACK_LEN;
        else if (s == LONG_PACK_START)  f->need = LONG_PACK_LEN;
        else {
            framer_drop_front(f, 1);
            return -1;
        }
    }
    if (f->have < f->need) return 0;
    *out_len = f->need;
    return 1;
}

/* =========================================================
 *  添加卡模式指示灯闪烁
 * ========================================================= */
static void manage_light_cb(void *arg);

static void light_blink_start(void)
{
    s_blink.running = 1;
    s_blink.on      = 0;
    DrvGpioCardLightSet(0);
    SvcTimerSet(TMR_MANAGE_LIGHT, LIGHT_BLINK_PERIOD_MS, manage_light_cb, NULL);
}

static void light_blink_stop(void)
{
    s_blink.running = 0;
    SvcTimerStop(TMR_MANAGE_LIGHT);
    DrvGpioCardLightSet(0);
}

static void manage_light_cb(void *arg)
{
    (void)arg;
    /* 两个条件都要满足才继续闪：模块自身未 stop + 业务层的 ADD_CARD 未超时 */
    if (!s_blink.running || !SvcTimerActive(TMR_ADD_CARD)) {
        light_blink_stop();
        return;
    }
    s_blink.on = !s_blink.on;
    DrvGpioCardLightSet(s_blink.on);
    SvcTimerSet(TMR_MANAGE_LIGHT, LIGHT_BLINK_PERIOD_MS, manage_light_cb, NULL);
}

/* =========================================================
 *  命令处理
 * ========================================================= */
static void handle_packet(PackType type, const unsigned char *buf, int len)
{
    (void)type; (void)len;
    unsigned char cmd  = buf[PKT_OFF_CMD];
    unsigned char arg1 = buf[PKT_OFF_ARG1];
    unsigned char arg2 = buf[PKT_OFF_ARG2];

    LOG_D("cmd=0x%02X arg1=%d arg2=%d", cmd, arg1, arg2);

    switch (cmd) {

    /* 进入添加卡模式：30s 超时，指示灯开始闪烁 */
    case NET_MGR_ADD_CARD:
        SvcTimerSet(TMR_ADD_CARD, ADD_CARD_TIMEOUT_MS, NULL, NULL);
        light_blink_start();
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
        } else {
            LOG_W("del card: arg1=%d out of range", arg1);
            break;
        }
        SvcVoicePlaySimple(VOICE_Bi2, VOICE_VOL_DEFAULT);
        break;

    /* 校验卡片（预留接口，暂无操作）*/
    case NET_MGR_VERIFY_CARD:
        break;

    /* 获取卡组权限列表，以长包形式回传 */
    case NET_MGR_GET_CARD: {
        unsigned char *deck_buf = NULL;
        int deck_len = AppCardDeckPermGet(&deck_buf);
        if (deck_len < 0 || deck_buf == NULL) {
            LOG_W("get deck fail: len=%d", deck_len);
            break;
        }
        LOG_I("send deck len=%d", deck_len);
        send_long(NET_MGR_GET_CARD, 0, deck_buf, deck_len);
        break;
    }

    /* 设置指定卡索引的权限位 */
    case NET_MGR_SET_CARD_PERM:
        if (arg1 >= DECK_SIZE_MAX) { LOG_W("set perm: idx=%d out of range", arg1); break; }
        AppCardSetPerm(arg1, arg2);
        LOG_I("set perm card[%d]=%d", arg1, arg2);
        break;

    /* 退出添加卡模式 */
    case NET_MGR_EXIT_CARD:
        SvcTimerStop(TMR_ADD_CARD);
        light_blink_stop();
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
}

/* =========================================================
 *  会话循环
 * ========================================================= */
static void session_cleanup(int cfd)
{
    ctx_set_session(-1, 0);
    close(cfd);
    SvcTimerStop(TMR_ADD_CARD);
    light_blink_stop();
    LOG_I("session end fd=%d", cfd);
}

static void session_run(int cfd)
{
    ctx_set_session(cfd, 1);
    LOG_I("session start fd=%d", cfd);

    RxFramer framer;
    framer_reset(&framer);

    struct timespec last_rx;
    clock_gettime(CLOCK_MONOTONIC, &last_rx);

    for (;;) {
        /* 空闲超时：长时间无数据即主动断开，防止死连接挂住 accept。
         * 与 TCP keepalive 互为双保险：keepalive 检测链路层断开，
         * 空闲超时覆盖对端假死（进程卡死但 TCP 连接健康）。 */
        if (ts_elapsed_ms(&last_rx) >= SESSION_IDLE_TIMEOUT_MS) {
            LOG_W("session idle > %dms, drop", SESSION_IDLE_TIMEOUT_MS);
            break;
        }

        int space = framer_free_space(&framer);
        if (space <= 0) {
            /* 理论上不可能：长包 = 1288 == 缓冲总大小，说明组帧状态被破坏 */
            LOG_E("framer buffer full, reset");
            framer_reset(&framer);
            space = framer_free_space(&framer);
        }

        unsigned char tmp[LONG_PACK_LEN];
        int cap = space < (int)sizeof(tmp) ? space : (int)sizeof(tmp);
        int n   = tcp_read_once(cfd, tmp, cap, RECV_TIMEOUT_MS);

        if (n == 0 || n == -2) break;       /* 对端关闭 / 异常 */
        if (n == -1) continue;              /* 本次超时 */

        clock_gettime(CLOCK_MONOTONIC, &last_rx);
        framer_append(&framer, tmp, n);

        /* 一次 read 可能包含多个包，循环解析到穷尽 */
        for (;;) {
            int pkt_len = 0;
            int r = framer_try_parse(&framer, &pkt_len);
            if (r == 0) break;              /* 需要更多数据 */
            if (r < 0) continue;            /* 起始字节无效，已丢 1 字节，重试 */

            PackType pt = check_packet(framer.buf, pkt_len);
            if (pt == PACK_INVALID) {
                LOG_W("bad packet, resync (have=%d)", framer.have);
                framer_drop_front(&framer, 1);   /* 丢 1 字节重新找起始 */
                continue;
            }
            handle_packet(pt, framer.buf, pkt_len);
            framer_drop_front(&framer, pkt_len); /* 消费本包 */
        }
    }

    session_cleanup(cfd);
}

static void *net_manage_thread(void *arg)
{
    (void)arg;
    LOG_I("thread start");

    while (1) {
        int cfd = tcp_accept(s_ctx.server_fd, ACCEPT_TIMEOUT_MS);
        if (cfd <= 0) continue;
        session_run(cfd);
    }
    return NULL;
}

/* =========================================================
 *  公共接口
 * ========================================================= */
int SvcNetManageConnected(void)
{
    pthread_mutex_lock(&s_ctx.lock);
    int v = s_ctx.connected;
    pthread_mutex_unlock(&s_ctx.lock);
    return v;
}

int SvcNetManageInit(void)
{
    /* 屏蔽 SIGPIPE，双重保险（send 也用了 MSG_NOSIGNAL）*/
    signal(SIGPIPE, SIG_IGN);

    s_ctx.server_fd = tcp_server_init(NET_MANAGE_PORT);
    if (s_ctx.server_fd < 0) {
        LOG_E("server init fail on port %d", NET_MANAGE_PORT);
        return -1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, net_manage_thread, NULL) != 0) {
        LOG_E("create thread fail: %s", strerror(errno));
        close(s_ctx.server_fd);
        s_ctx.server_fd = -1;
        return -1;
    }
    pthread_detach(tid);
    LOG_I("server ready on port %d", NET_MANAGE_PORT);
    return 0;
}
