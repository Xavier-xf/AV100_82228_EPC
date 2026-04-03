/**
 * @file    svc_intercom_stream.c
 * @brief   对讲媒体流服务
 */
#include "svc_intercom_stream.h"
#include "svc_timer.h"
#include "event_bus.h"
#include "drv_audio_in.h"
#include "drv_audio_out.h"
#include "drv_video_in.h"
#include "drv_gpio.h"
#include "g711_table.h"
#include "drv_net_raw.h"
#include "svc_network.h"
#include "svc_audio.h"

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* =========================================================
 *  配置常量
 * ========================================================= */
#define NET_IFACE              "eth0"
#define AUDIO_NET_PROTO_BASE   0x2600u
#define VIDEO_NET_PROTO_BASE   0x1600u
#define VIDEO_BCAST_ADDR       "255.255.255.255"
#define MQ_KEY_AUDIO_TX        ((key_t)0xDA01)
#define MQ_KEY_AUDIO_RX        ((key_t)0xDA02)
#define MQ_KEY_VIDEO_TX        ((key_t)0xDA03)  
#define AUDIO_MSG_PCM_MAX      4096
#define VIDEO_POOL_SLOTS       4
#define VIDEO_FRAME_MAX        (128 * 1024)
#define AUDIO_PKG_MAX          1510
#define VIDEO_PKG_MAX          (32 * 1024)
#define STREAM_WATCHDOG_MS     5000

/* =========================================================
 *  消息结构
 * ========================================================= */
typedef struct { long mtype; uint32_t len; uint8_t pcm[AUDIO_MSG_PCM_MAX]; } AudioTxMsg;
typedef struct { long mtype; uint32_t len; uint8_t pcm[AUDIO_MSG_PCM_MAX]; } AudioRxMsg;
typedef struct { long mtype; int slot_idx; } VideoTxMsg;

#define AUDIO_MSG_BODY  (sizeof(AudioTxMsg) - sizeof(long))
#define AUDIO_RX_BODY   (sizeof(AudioRxMsg) - sizeof(long))
#define VIDEO_MSG_BODY  (sizeof(VideoTxMsg) - sizeof(long))

/* =========================================================
 *  视频帧共享池（用互斥锁保护 in_use 状态）
 * ========================================================= */
typedef struct {
    pthread_mutex_t lock;
    int             in_use;    /* 0=空闲 1=写入中 2=待发送 */
    uint8_t         data[VIDEO_FRAME_MAX];
    uint32_t        len;
    uint64_t        pts_ms;
    uint32_t        frame_idx;
} VideoSlot;

static VideoSlot s_video_pool[VIDEO_POOL_SLOTS];

/* =========================================================
 *  模块状态结构体（所有字段由 s_stm.lock 保护）
 * ========================================================= */
typedef struct {
    pthread_mutex_t   lock;
    int               active;             /* 流是否激活 */
    int               threads_running;    /* 工作线程运行标志 */
    StreamMode        mode;
    uint8_t           peer_dev_id;
    /* 消息队列句柄 */
    int               mq_audio_tx;
    int               mq_audio_rx;
    int               mq_video_tx;
    /* Socket 句柄（sock_lock 单独保护发包地址，lock 保护 fd）*/
    pthread_mutex_t   sock_lock;
    int               audio_tx_fd;
    int               audio_rx_fd;
    int               video_tx_fd;
    struct sockaddr_ll audio_tx_addr;
    struct sockaddr_in video_tx_addr;
} SvcStreamCtx;

static SvcStreamCtx s_stm = {
    .lock            = PTHREAD_MUTEX_INITIALIZER,
    .active          = 0,
    .threads_running = 0,
    .mode            = STREAM_MODE_TALK,
    .peer_dev_id     = 1,
    .mq_audio_tx     = -1,
    .mq_audio_rx     = -1,
    .mq_video_tx     = -1,
    .sock_lock       = PTHREAD_MUTEX_INITIALIZER,
    .audio_tx_fd     = -1,
    .audio_rx_fd     = -1,
    .video_tx_fd     = -1,
};

/* 起始码 */
static const uint8_t AUDIO_START_CODE[4] = {0x00, 0x00, 0x01, 0xFC};
static const uint8_t VIDEO_START_CODE[4] = {0x00, 0x00, 0x01, 0xFC};

/* ---- 辅助：加锁读状态 ---- */
static int  get_active(void)           { pthread_mutex_lock(&s_stm.lock); int v=s_stm.active;          pthread_mutex_unlock(&s_stm.lock); return v; }
static int  get_threads_running(void)  { pthread_mutex_lock(&s_stm.lock); int v=s_stm.threads_running; pthread_mutex_unlock(&s_stm.lock); return v; }
static int  get_mq_audio_tx(void)     { pthread_mutex_lock(&s_stm.lock); int v=s_stm.mq_audio_tx;     pthread_mutex_unlock(&s_stm.lock); return v; }
static int  get_mq_audio_rx(void)     { pthread_mutex_lock(&s_stm.lock); int v=s_stm.mq_audio_rx;     pthread_mutex_unlock(&s_stm.lock); return v; }
static int  get_mq_video_tx(void)     { pthread_mutex_lock(&s_stm.lock); int v=s_stm.mq_video_tx;     pthread_mutex_unlock(&s_stm.lock); return v; }

/* =========================================================
 *  工具
 * ========================================================= */
static int get_ifindex(int fd, const char *iface)
{
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("[SvcStream] SIOCGIFINDEX"); return -1; }
    return ifr.ifr_ifindex;
}

static int mq_create(key_t key)
{
    int old = msgget(key, 0600);
    if (old >= 0) { msgctl(old, IPC_RMID, NULL); }
    int id = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (id < 0) { perror("[SvcStream] msgget"); return -1; }
    struct msqid_ds qa;
    if (msgctl(id, IPC_STAT, &qa) == 0) {
        qa.msg_qbytes = (unsigned long)(AUDIO_MSG_PCM_MAX * 8);
        msgctl(id, IPC_SET, &qa);
    }
    return id;
}

static void mq_destroy(int *id)
{
    if (*id >= 0) { msgctl(*id, IPC_RMID, NULL); *id = -1; }
}

/* =========================================================
 *  网络套接字
 * ========================================================= */
static int audio_net_proto(uint8_t dev_id)
{
    int slave = (dev_id - 1) & 0x0F;
    return (int)(AUDIO_NET_PROTO_BASE | ((unsigned)slave << 4) | (unsigned)slave);
}

static int video_net_port(uint8_t dev_id)
{
    return (int)(VIDEO_NET_PROTO_BASE | ((dev_id - 1) & 0x0Fu));
}

/* ★ 音频协议号用本机 ID
 * 例：本机 ID=0x07 → proto = 0x2666 */
static int open_audio_tx_socket(void)
{
    uint8_t local_id = SvcNetworkLocalDeviceGet();
    int proto = audio_net_proto(local_id);
    printf("[SvcStream] audio_tx proto=0x%04X (local_dev=0x%02X)\n", proto, local_id);

    int fd = socket(PF_PACKET, SOCK_RAW, htons((uint16_t)proto));
    if (fd < 0) { perror("[SvcStream] audio_tx socket"); return -1; }
    int ifidx = get_ifindex(fd, NET_IFACE);
    if (ifidx < 0) { close(fd); return -1; }

    pthread_mutex_lock(&s_stm.sock_lock);
    memset(&s_stm.audio_tx_addr, 0, sizeof(s_stm.audio_tx_addr));
    s_stm.audio_tx_addr.sll_family  = AF_PACKET;
    s_stm.audio_tx_addr.sll_ifindex = ifidx;
    /* sll_halen=0：由 NetRawPacketHead 手动填充完整 MAC 头 */
    pthread_mutex_unlock(&s_stm.sock_lock);
    return fd;
}

static int open_audio_rx_socket(void)
{
    uint8_t local_id = SvcNetworkLocalDeviceGet();
    int proto = audio_net_proto(local_id);
    printf("[SvcStream] audio_rx proto=0x%04X (local_dev=0x%02X)\n", proto, local_id);

    int fd = socket(PF_PACKET, SOCK_RAW, htons((uint16_t)proto));
    if (fd < 0) { perror("[SvcStream] audio_rx socket"); return -1; }
    int ifidx = get_ifindex(fd, NET_IFACE);
    if (ifidx < 0) { close(fd); return -1; }
    struct sockaddr_ll sll = {
        .sll_family   = AF_PACKET,
        .sll_protocol = htons((uint16_t)proto),
        .sll_ifindex  = ifidx
    };
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("[SvcStream] audio_rx bind"); close(fd); return -1;
    }
    int rcvbuf = 10 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    return fd;
}

/* ★ 视频端口用本机 ID
 * 例：本机 ID=0x07 → port = 0x1606 */
static int open_video_tx_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[SvcStream] video_tx socket"); return -1; }
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, NET_IFACE, IFNAMSIZ - 1);
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr));
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    uint8_t local_id = SvcNetworkLocalDeviceGet();
    int port = video_net_port(local_id);
    printf("[SvcStream] video port=0x%04X (local_dev=0x%02X)\n", port, local_id);

    pthread_mutex_lock(&s_stm.sock_lock);
    memset(&s_stm.video_tx_addr, 0, sizeof(s_stm.video_tx_addr));
    s_stm.video_tx_addr.sin_family      = AF_INET;
    s_stm.video_tx_addr.sin_addr.s_addr = inet_addr(VIDEO_BCAST_ADDR);
    s_stm.video_tx_addr.sin_port        = htons((uint16_t)port);
    pthread_mutex_unlock(&s_stm.sock_lock);
    return fd;
}

/* =========================================================
 *  网络发包
 * ========================================================= */
#define AUDIO_PAYLOAD_OFFSET 60
#define AUDIO_HDR_LEN        17
#define VIDEO_HDR_LEN        17

static void net_audio_send(int fd, const uint8_t *alaw, uint32_t alaw_size)
{
    /* 缓冲区布局：
     *   buf[0..59]  = 以太网 MAC 头（由 NetRawPacketHead 每包填充）
     *   buf[60..76] = 音频起始码(4B) + 帧总大小(4B) + PTS(4B) + 帧序号(4B) + 帧类型(1B)
     *   buf[77..]   = G711 alaw 音频数据
     * 后续分片：
     *   buf[0..59]  = MAC 头（重新填充）
     *   buf[60..]   = 剩余音频数据
     */
    static uint8_t *buf = NULL;
    if (!buf) { buf = malloc(AUDIO_PKG_MAX); if (!buf) return; }

    static uint32_t frame_idx = 0; frame_idx++;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t pts = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    pthread_mutex_lock(&s_stm.sock_lock);
    struct sockaddr_ll addr = s_stm.audio_tx_addr;
    /* ★ 协议号用本机 ID（与 open_audio_tx_socket 一致）*/
    int proto = audio_net_proto(SvcNetworkLocalDeviceGet());
    pthread_mutex_unlock(&s_stm.sock_lock);

    uint32_t remain = alaw_size, sent = 0;
    int first = 1;
    while (remain > 0) {
        /* 每包均重新填充 MAC 头*/
        NetRawPacketHead(buf, NET_IFACE, proto);

        if (first) {
            first = 0;
            /* 音频起始码 + 帧头（偏移 60）*/
            memcpy(&buf[AUDIO_PAYLOAD_OFFSET], AUDIO_START_CODE, 4);
            buf[AUDIO_PAYLOAD_OFFSET+4]=(uint8_t)((alaw_size>>24)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+5]=(uint8_t)((alaw_size>>16)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+6]=(uint8_t)((alaw_size>>8)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+7]=(uint8_t)(alaw_size&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+8]=(uint8_t)((pts>>24)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+9]=(uint8_t)((pts>>16)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+10]=(uint8_t)((pts>>8)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+11]=(uint8_t)(pts&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+12]=(uint8_t)((frame_idx>>24)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+13]=(uint8_t)((frame_idx>>16)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+14]=(uint8_t)((frame_idx>>8)&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+15]=(uint8_t)(frame_idx&0xFF);
            buf[AUDIO_PAYLOAD_OFFSET+16]=0;   /* FrameType = PCM */

            int max_data = AUDIO_PKG_MAX - AUDIO_PAYLOAD_OFFSET - AUDIO_HDR_LEN;
            uint32_t copy = (remain > (uint32_t)max_data) ? (uint32_t)max_data : remain;
            memcpy(&buf[AUDIO_PAYLOAD_OFFSET + AUDIO_HDR_LEN], alaw + sent, copy);
            sendto(fd, buf, AUDIO_PAYLOAD_OFFSET + AUDIO_HDR_LEN + (int)copy, 0,
                   (struct sockaddr *)&addr, sizeof(addr));
            sent += copy; remain -= copy;
        } else {
            /* 后续分片：直接发剩余数据，无起始码*/
            uint32_t copy = remain;
            if (copy > (uint32_t)(AUDIO_PKG_MAX - AUDIO_PAYLOAD_OFFSET))
                copy = (uint32_t)(AUDIO_PKG_MAX - AUDIO_PAYLOAD_OFFSET);
            memcpy(&buf[AUDIO_PAYLOAD_OFFSET], alaw + sent, copy);
            sendto(fd, buf, AUDIO_PAYLOAD_OFFSET + (int)copy, 0,
                   (struct sockaddr *)&addr, sizeof(addr));
            sent += copy; remain -= copy;
        }
        usleep(1000);
    }
}

static void net_video_send(int fd, const uint8_t *data, uint32_t size,
                            uint64_t pts, uint32_t frame_idx)
{
    static uint8_t *buf = NULL;
    if (!buf) { buf = malloc(VIDEO_PKG_MAX); if (!buf) return; }

    pthread_mutex_lock(&s_stm.sock_lock);
    struct sockaddr_in addr = s_stm.video_tx_addr;
    pthread_mutex_unlock(&s_stm.sock_lock);

    uint32_t remain = size, sent = 0;
    int first = 1;
    while (remain > 0) {
        if (first) {
            first = 0;
            memcpy(buf, VIDEO_START_CODE, 4);
            buf[4]=(size>>24)&0xFF; buf[5]=(size>>16)&0xFF;
            buf[6]=(size>>8)&0xFF;  buf[7]=size&0xFF;
            buf[8]=(pts>>24)&0xFF;  buf[9]=(pts>>16)&0xFF;
            buf[10]=(pts>>8)&0xFF;  buf[11]=pts&0xFF;
            buf[12]=(frame_idx>>24)&0xFF; buf[13]=(frame_idx>>16)&0xFF;
            buf[14]=(frame_idx>>8)&0xFF;  buf[15]=frame_idx&0xFF;
            buf[16]=0;
            uint32_t copy = remain;
            if ((int)copy > (VIDEO_PKG_MAX - VIDEO_HDR_LEN)) copy = (uint32_t)(VIDEO_PKG_MAX - VIDEO_HDR_LEN);
            memcpy(&buf[VIDEO_HDR_LEN], data + sent, copy);
            sendto(fd, buf, VIDEO_HDR_LEN + (int)copy, 0,
                   (struct sockaddr *)&addr, sizeof(addr));
            sent += copy; remain -= copy;
        } else {
            uint32_t copy = (remain > (uint32_t)VIDEO_PKG_MAX) ? (uint32_t)VIDEO_PKG_MAX : remain;
            memcpy(buf, data + sent, copy);
            sendto(fd, buf, (int)copy, 0, (struct sockaddr *)&addr, sizeof(addr));
            sent += copy; remain -= copy;
        }
        usleep(1000);
    }
}

/* =========================================================
 *  工作线程
 * ========================================================= */
static void *audio_tx_thread(void *arg)
{
    (void)arg;
    AudioTxMsg msg;
    pcm16_alaw_tableinit();
    while (get_threads_running()) {
        int mqid = get_mq_audio_tx();
        if (mqid < 0) { usleep(10*1000); continue; }
        if (msgrcv(mqid, &msg, AUDIO_MSG_BODY, 1, 0) <= 0) continue;
        if (!get_active()) continue;

        pthread_mutex_lock(&s_stm.lock);
        int fd = s_stm.audio_tx_fd;
        pthread_mutex_unlock(&s_stm.lock);
        if (fd < 0) continue;

        uint32_t alaw_len = msg.len / 2;
        uint8_t *alaw = malloc(alaw_len);
        if (!alaw) continue;
        pcm16_to_alaw(msg.len, (const char *)msg.pcm, (char *)alaw);
        net_audio_send(fd, alaw, alaw_len);
        free(alaw);
    }
    printf("[SvcStream] audio_tx_thread exit\n");
    return NULL;
}

/**
 * @brief 对讲音频接收处理
 * 关键：累积满 2048 字节 G711（= 4096 字节 PCM）再一次性丢入 AO，
 * 而不是每包立即解码，避免 AO 每次写入量太小导致音频断续。
 */
#define INTERCOM_ALAW_ACCUM_SIZE  2048   

static void audio_rx_handle(const uint8_t *alaw, uint32_t alaw_size)
{
    /* 静态积累缓冲*/
    static unsigned char accum_buf[INTERCOM_ALAW_ACCUM_SIZE];
    static unsigned int  accum_len = 0;

    int mqid = get_mq_audio_rx();
    if (mqid < 0) return;

    uint32_t offset = 0;
    while (offset < alaw_size) {
        /* 本次能拷入的字节数 */
        uint32_t space   = (uint32_t)(INTERCOM_ALAW_ACCUM_SIZE - accum_len);
        uint32_t to_copy = alaw_size - offset;
        if (to_copy > space) to_copy = space;

        memcpy(&accum_buf[accum_len], alaw + offset, to_copy);
        accum_len += to_copy;
        offset    += to_copy;

        /* 累积满 2048 字节*/
        if (accum_len >= INTERCOM_ALAW_ACCUM_SIZE) {
            uint32_t pcm_len = accum_len * 2;   /* G711→PCM 大小翻倍 */
            if (pcm_len > AUDIO_MSG_PCM_MAX) pcm_len = AUDIO_MSG_PCM_MAX;

            AudioRxMsg msg;
            msg.mtype = 1;
            msg.len   = pcm_len;
            /* alaw_to_pcm16：解压 accum_len 字节 G711 → pcm_len 字节 PCM */
            alaw_to_pcm16(accum_len, (const char *)accum_buf, (char *)msg.pcm);

            if (msgsnd(mqid, &msg, AUDIO_RX_BODY, IPC_NOWAIT) < 0) {
                /* 队列满丢弃*/
            }
            accum_len = 0;
        }
    }
}

static void *audio_rx_thread(void *arg)
{
    (void)arg;
    alaw_pcm16_tableinit();
    uint8_t recv_buf[2048];
    struct { int valid; uint32_t total; uint32_t got; uint8_t buf[AUDIO_MSG_PCM_MAX*2]; } rx = {0};

    while (get_threads_running()) {
        pthread_mutex_lock(&s_stm.lock);
        int active = s_stm.active; int fd = s_stm.audio_rx_fd;
        pthread_mutex_unlock(&s_stm.lock);
        if (!active || fd < 0) { usleep(10*1000); continue; }

        /* 使用 NetRawPacketReceive 接收
         * 返回有效载荷长度，timeout=5ms*/
        int len = NetRawPacketReceive(fd, recv_buf, sizeof(recv_buf), 5);
        if (len <= 0) continue;

        uint8_t *p = recv_buf; int left = len;
        while (left > 0) {
            if (memcmp(p, AUDIO_START_CODE, 4) == 0 && left >= 17) {
                rx.valid = 1;
                rx.total = (uint32_t)((p[4]<<24)|(p[5]<<16)|(p[6]<<8)|p[7]);
                rx.got   = 0;
                if (rx.total > sizeof(rx.buf)) rx.total = (uint32_t)sizeof(rx.buf);
                p += 17; left -= 17; continue;
            }
            if (rx.valid && left > 0) {
                uint32_t copy = (uint32_t)left;
                if (rx.got + copy > rx.total) copy = rx.total - rx.got;
                memcpy(&rx.buf[rx.got], p, copy);
                rx.got += copy; p += copy; left -= (int)copy;
                if (rx.got >= rx.total) { audio_rx_handle(rx.buf, rx.total); rx.valid = 0; }
            } else break;
        }
    }
    printf("[SvcStream] audio_rx_thread exit\n");
    return NULL;
}

/* audio_spk_thread：通过 SvcAudioFeed 统一输出，不直接调 DrvAudioOutWrite。
 * 这样 AO 只有一个消费者（svc_audio.c audio_output_thread），避免并发写入。
 * mtype=AUDIO_SRC_INTERCOM=2，优先级低于语音播报（mtype=1）。*/
static void *audio_spk_thread(void *arg)
{
    (void)arg;
    AudioRxMsg msg;
    while (get_threads_running()) {
        int mqid = get_mq_audio_rx();
        if (mqid < 0) { usleep(10*1000); continue; }
        if (msgrcv(mqid, &msg, AUDIO_RX_BODY, 1, 0) <= 0) continue;
        if (!get_active()) continue;
        /* 走 svc_audio 统一输出，不再直接写 AO */
        SvcAudioFeed(AUDIO_SRC_INTERCOM, msg.pcm, msg.len);
    }
    printf("[SvcStream] audio_spk_thread exit\n");
    return NULL;
}

static void *video_tx_thread(void *arg)
{
    (void)arg;
    VideoTxMsg msg;
    while (get_threads_running()) {
        int mqid = get_mq_video_tx();
        if (mqid < 0) { usleep(10*1000); continue; }
        if (msgrcv(mqid, &msg, VIDEO_MSG_BODY, 1, 0) <= 0) continue;

        int idx = msg.slot_idx;
        if (idx < 0 || idx >= VIDEO_POOL_SLOTS) continue;

        VideoSlot *slot = &s_video_pool[idx];
        pthread_mutex_lock(&slot->lock);
        int in_use = slot->in_use;
        pthread_mutex_unlock(&slot->lock);

        pthread_mutex_lock(&s_stm.lock);
        int active = s_stm.active; int fd = s_stm.video_tx_fd;
        pthread_mutex_unlock(&s_stm.lock);

        if (active && fd >= 0 && in_use == 2) {
            pthread_mutex_lock(&slot->lock);
            uint32_t len  = slot->len;
            uint64_t pts  = slot->pts_ms;
            uint32_t fidx = slot->frame_idx;
            pthread_mutex_unlock(&slot->lock);
            net_video_send(fd, slot->data, len, pts, fidx);
        }

        pthread_mutex_lock(&slot->lock);
        slot->in_use = 0;
        pthread_mutex_unlock(&slot->lock);
    }
    printf("[SvcStream] video_tx_thread exit\n");
    return NULL;
}

/* =========================================================
 *  drv_audio_in 回调
 * ========================================================= */
static void on_audio_in_frame(const AudioInFrame *frame)
{
    pthread_mutex_lock(&s_stm.lock);
    int active = s_stm.active;
    StreamMode mode = s_stm.mode;
    int mqid = s_stm.mq_audio_tx;
    pthread_mutex_unlock(&s_stm.lock);

    /* 旧版 network_audio_send_package_start() 无条件启动，监控和通话模式均发送
     * 室外麦克风音频给室内机，室内机才能在监控/留言模式下录到访客声音。*/
    if (!active || mqid < 0) return;
    (void)mode;

    AudioTxMsg msg;
    msg.mtype = 1;
    msg.len   = (frame->len > AUDIO_MSG_PCM_MAX) ? AUDIO_MSG_PCM_MAX : frame->len;
    memcpy(msg.pcm, frame->data, msg.len);
    msgsnd(mqid, &msg, AUDIO_MSG_BODY, IPC_NOWAIT);
}

/* =========================================================
 *  drv_video_in 回调（视频帧 → 槽位 → 消息队列）
 * ========================================================= */
static void on_video_frame(const VideoFrame *frame)
{
    pthread_mutex_lock(&s_stm.lock);
    int active = s_stm.active; int mqid = s_stm.mq_video_tx;
    pthread_mutex_unlock(&s_stm.lock);

    if (!active || mqid < 0) return;
    if (frame->len > VIDEO_FRAME_MAX) { printf("[SvcStream] video frame too large\n"); return; }

    for (int i = 0; i < VIDEO_POOL_SLOTS; i++) {
        pthread_mutex_lock(&s_video_pool[i].lock);
        if (s_video_pool[i].in_use == 0) {
            s_video_pool[i].in_use = 1;
            pthread_mutex_unlock(&s_video_pool[i].lock);

            memcpy(s_video_pool[i].data, frame->data, frame->len);
            pthread_mutex_lock(&s_video_pool[i].lock);
            s_video_pool[i].len       = frame->len;
            s_video_pool[i].pts_ms    = frame->pts_ms;
            s_video_pool[i].frame_idx = frame->frame_idx;
            s_video_pool[i].in_use    = 2;
            pthread_mutex_unlock(&s_video_pool[i].lock);

            VideoTxMsg msg = {.mtype = 1, .slot_idx = i};
            if (msgsnd(mqid, &msg, VIDEO_MSG_BODY, IPC_NOWAIT) < 0) {
                pthread_mutex_lock(&s_video_pool[i].lock);
                s_video_pool[i].in_use = 0;
                pthread_mutex_unlock(&s_video_pool[i].lock);
            }
            return;
        }
        pthread_mutex_unlock(&s_video_pool[i].lock);
    }
    printf("[SvcStream] video pool full, drop frame\n");
}

/* =========================================================
 *  流看门狗
 * ========================================================= */
static void on_stream_watchdog(void *arg)
{
    (void)arg;
    printf("[SvcStream] watchdog timeout!\n");
    EventBusPublish(EVT_INTERCOM_STREAM_WATCHDOG, NULL, 0);
}

/* =========================================================
 *  启动线程
 * ========================================================= */
static int start_threads(void)
{
    pthread_mutex_lock(&s_stm.lock);
    s_stm.threads_running = 1;
    pthread_mutex_unlock(&s_stm.lock);

    const struct { void *(*fn)(void*); } tbl[] = {
        {audio_tx_thread}, {audio_rx_thread}, {audio_spk_thread}, {video_tx_thread}
    };
    for (int i = 0; i < (int)(sizeof(tbl)/sizeof(tbl[0])); i++) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, tbl[i].fn, NULL) != 0) {
            printf("[SvcStream] create thread %d fail\n", i); return -1;
        }
        pthread_detach(tid);
    }
    return 0;
}

/* =========================================================
 *  公开接口
 * ========================================================= */
int SvcIntercomStreamStart(StreamMode mode, uint8_t peer_dev_id)
{
    pthread_mutex_lock(&s_stm.lock);
    if (s_stm.active) { pthread_mutex_unlock(&s_stm.lock); return 0; }
    s_stm.mode       = mode;
    s_stm.peer_dev_id = peer_dev_id;
    pthread_mutex_unlock(&s_stm.lock);

    printf("[SvcStream] start mode=%d peer=0x%02X\n", mode, peer_dev_id);

    pcm16_alaw_tableinit();
    alaw_pcm16_tableinit();

    int atx = open_audio_tx_socket();
    int arx = open_audio_rx_socket();
    int vtx = open_video_tx_socket();
    if (atx < 0 || arx < 0 || vtx < 0) { goto fail_sock; }

    int mq_atx = mq_create(MQ_KEY_AUDIO_TX);
    int mq_arx = mq_create(MQ_KEY_AUDIO_RX);
    int mq_vtx = mq_create(MQ_KEY_VIDEO_TX);
    if (mq_atx < 0 || mq_arx < 0 || mq_vtx < 0) { goto fail_mq; }

    /* 清空视频池 */
    for (int i = 0; i < VIDEO_POOL_SLOTS; i++) {
        pthread_mutex_lock(&s_video_pool[i].lock);
        s_video_pool[i].in_use = 0;
        pthread_mutex_unlock(&s_video_pool[i].lock);
    }

    pthread_mutex_lock(&s_stm.lock);
    s_stm.audio_tx_fd = atx; s_stm.audio_rx_fd = arx; s_stm.video_tx_fd = vtx;
    s_stm.mq_audio_tx = mq_atx; s_stm.mq_audio_rx = mq_arx; s_stm.mq_video_tx = mq_vtx;
    s_stm.active = 1;
    pthread_mutex_unlock(&s_stm.lock);

    DrvVideoInStart();
    DrvAudioInStart();
    SvcTimerSet(TMR_INTERCOM_WATCHDOG, STREAM_WATCHDOG_MS, on_stream_watchdog, NULL);

    printf("[SvcStream] started ok\n");
    return 0;

fail_mq:
    if (mq_atx >= 0) msgctl(mq_atx, IPC_RMID, NULL);
    if (mq_arx >= 0) msgctl(mq_arx, IPC_RMID, NULL);
    if (mq_vtx >= 0) msgctl(mq_vtx, IPC_RMID, NULL);
fail_sock:
    if (atx >= 0) close(atx);
    if (arx >= 0) close(arx);
    if (vtx >= 0) close(vtx);
    return -1;
}

int SvcIntercomStreamStop(void)
{
    if (!get_active()) return 0;
    printf("[SvcStream] stopping...\n");

    DrvAudioInStop();
    DrvVideoInStop();
    SvcTimerStop(TMR_INTERCOM_WATCHDOG);

    pthread_mutex_lock(&s_stm.lock);
    s_stm.active = 0;
    int atx = s_stm.audio_tx_fd; int arx = s_stm.audio_rx_fd; int vtx = s_stm.video_tx_fd;
    s_stm.audio_tx_fd = -1; s_stm.audio_rx_fd = -1; s_stm.video_tx_fd = -1;
    pthread_mutex_unlock(&s_stm.lock);

    usleep(200 * 1000);

    if (atx >= 0) close(atx);
    if (arx >= 0) close(arx);
    if (vtx >= 0) close(vtx);

    pthread_mutex_lock(&s_stm.lock);
    mq_destroy(&s_stm.mq_audio_tx);
    mq_destroy(&s_stm.mq_audio_rx);
    mq_destroy(&s_stm.mq_video_tx);
    pthread_mutex_unlock(&s_stm.lock);

    /* 清空残留的对讲音频，避免下次启动时播出旧数据 */
    SvcAudioFlush(AUDIO_SRC_INTERCOM);
    DrvGpioAmpDisable();
    printf("[SvcStream] stopped\n");
    return 0;
}

int SvcIntercomStreamActive(void) { return get_active(); }
void SvcIntercomStreamUpgradeToTalk(void)
{
    pthread_mutex_lock(&s_stm.lock);
    s_stm.mode = STREAM_MODE_TALK;
    pthread_mutex_unlock(&s_stm.lock);
    
    DrvGpioAmpEnable();
    printf("[SvcStream] upgraded to TALK mode\n");
}
void SvcIntercomStreamRefresh(const void *status)
{
    const NetStreamStatus *s = (const NetStreamStatus *)status;
    SvcTimerRefresh(TMR_INTERCOM_WATCHDOG, STREAM_WATCHDOG_MS);
    if (s) {
        /* audio_volume 在 svc_network.c 中已换算（arg1*3+66），直接使用 */
        DrvAudioOutSetVolume((int)s->audio_volume);
    }
}

void SvcIntercomStreamSetVolume(uint8_t raw_vol)
{
    /* raw_vol 是原始网络参数（未换算），此处换算一次*/
    DrvAudioOutSetVolume((int)raw_vol * 3 + 66);
}

void SvcIntercomStreamRequestKeyFrame(void) { DrvVideoInRequestIdr(); }


int SvcIntercomStreamInit(void)
{
    /* 初始化视频池互斥锁 */
    for (int i = 0; i < VIDEO_POOL_SLOTS; i++) {
        memset(&s_video_pool[i], 0, sizeof(VideoSlot));
        pthread_mutex_init(&s_video_pool[i].lock, NULL);
    }

    DrvAudioInSetCallback(on_audio_in_frame);
    DrvVideoInSetCallback(on_video_frame);

    if (start_threads() != 0) return -1;
    printf("[SvcStream] init ok\n");
    return 0;
}
