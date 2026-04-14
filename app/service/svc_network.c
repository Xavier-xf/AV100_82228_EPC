/**
 * @file    svc_network.c
 * @brief   命令通道网络服务
 *
 * 职责：命令帧打包/分发，设备 ID 管理，EventBus 发布，
 *        接收线程（含 eth0 异常复位），对外发送接口。
 *
 * 帧格式（8 字节短包）：
 *   [0xAA][src][dst][cmd][arg1][arg2][sum][0x55]
 *   sum = (src+dst+cmd+arg1+arg2) & 0xFF
 *
 * 长包（DataLen > 8，升级数据）：
 *   [0xAA][src][dst][0x62][DP[0..n]][sum][0x55]
 *   DP[0..3]=包序号, DP[4..7]=数据长度, DP[8..]=数据
 *
 * 命令 ID：
 *   0x54 UnlockEvent           远程开锁
 *   0x55 IdRepeatEvent         心跳
 *   0x56 DoorbellEvent         门铃
 *   0x57 OutdoorTalkEvent      室内机接听
 *   0x59 StreamStatusEvent     流控/监控保活
 *   0x61 MotionDelectEvent     移动侦测通知
 *   0x62 UpgraedOutdoorEvent   固件升级
 *   0x63 MotionSensitivityEvent
 *   0x65 OutdoorResetEvent     出厂复位
 *   0x70 CompileTimeEvent      版本信息
 *   0x75 OutdoorHangEvent      挂断
 *
 * 设备 ID：
 *   1~6=室内机, 7=OUTDOOR_1, 8=OUTDOOR_2, 0xFF=广播
 */
#define LOG_TAG "SvcNet"
#include "log.h"

#include "svc_network.h"
#include "drv_net_raw.h"
#include "event_bus.h"
#include "svc_svp.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

/* =========================================================
 *  协议常量
 * ========================================================= */
#define NET_IFACE        "eth0"
#define ETH_P_CMD        0xFFFF
#define NET_CMD_START    0xAA
#define NET_CMD_END      0x55
#define NET_PACK_LEN     8

#define DEVICE_OUTDOOR_1 7
#define DEVICE_OUTDOOR_2 8
#define DEVICE_ALL       0xFF

typedef enum {
    CMD_LIGHT_EVENT   = 0x53,
    CMD_UNLOCK        = 0x54,
    CMD_ID_REPEAT     = 0x55,
    CMD_DOORBELL      = 0x56,
    CMD_OUTDOOR_TALK  = 0x57,
    CMD_STREAM_STATUS = 0x59,
    CMD_DEVICE_BUSY   = 0x60,
    CMD_MOTION_DETECT = 0x61,
    CMD_UPGRADE       = 0x62,
    CMD_MOTION_SENS   = 0x63,
    CMD_ADD_DEL_CARD  = 0x64,
    CMD_OUTDOOR_RESET = 0x65,
    CMD_COMPILE_TIME  = 0x70,
    CMD_EXIT_BTN_TIME = 0x72,
    CMD_OUTDOOR_HANG  = 0x75,
    CMD_GATE2_UNLOCK  = 0x99,
} NetCmdId;

/* =========================================================
 *  模块状态结构体
 * ========================================================= */
typedef struct {
    pthread_mutex_t  send_mutex;
    pthread_mutex_t  state_lock;
    int              send_fd;
    int              recv_fd;
    struct sockaddr_ll send_sll;
    uint8_t          local_dev;
    int              running;
} SvcNetCtx;

static SvcNetCtx s_net = {
    .send_mutex = PTHREAD_MUTEX_INITIALIZER,
    .state_lock = PTHREAD_MUTEX_INITIALIZER,
    .send_fd    = -1,
    .recv_fd    = -1,
    .local_dev  = DEVICE_OUTDOOR_1,
    .running    = 0,
};

static uint8_t get_local_dev(void)
{
    pthread_mutex_lock(&s_net.state_lock);
    uint8_t v = s_net.local_dev;
    pthread_mutex_unlock(&s_net.state_lock);
    return v;
}

/* =========================================================
 *  打包与发送
 * ========================================================= */
static void net_code_pack(uint8_t dst_dev, uint8_t cmd,
                           uint8_t arg1, uint8_t arg2, uint8_t *out)
{
    uint8_t src = get_local_dev();
    out[0] = NET_CMD_START;
    out[1] = src;
    out[2] = dst_dev;
    out[3] = cmd;
    out[4] = arg1;
    out[5] = arg2;
    out[6] = (uint8_t)((out[1]+out[2]+out[3]+out[4]+out[5]) & 0xFF);
    out[7] = NET_CMD_END;
}

static void net_msg_send(uint8_t dst, uint8_t cmd, uint8_t arg1, uint8_t arg2)
{
    uint8_t buf[NET_PACK_LEN];
    net_code_pack(dst, cmd, arg1, arg2, buf);

    pthread_mutex_lock(&s_net.send_mutex);
    pthread_mutex_lock(&s_net.state_lock);
    int fd = s_net.send_fd;
    struct sockaddr_ll sll = s_net.send_sll;
    pthread_mutex_unlock(&s_net.state_lock);

    if (fd >= 0)
        NetRawPacketSend(fd, &sll, buf, NET_PACK_LEN, NET_IFACE, ETH_P_CMD);

    pthread_mutex_unlock(&s_net.send_mutex);
}

/* =========================================================
 *  eth0 软件 up/down（接收异常时复位网口）
 * ========================================================= */
static int eth_interface_state(const char *iface, int enable)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { LOG_E("socket fail"); return -1; }

    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        LOG_E("SIOCGIFFLAGS fail"); close(fd); return -1;
    }
    if (enable) ifr.ifr_flags |=  IFF_UP;
    else        ifr.ifr_flags &= (short)~IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        LOG_E("SIOCSIFFLAGS fail"); close(fd); return -1;
    }
    LOG_I("eth %s %s", iface, enable ? "up" : "down");
    close(fd);
    return 0;
}

/* =========================================================
 *  命令帧分发
 * ========================================================= */
static void raw_packet_handle(const uint8_t *buf, int len)
{
    if (len < NET_PACK_LEN)           return;
    if (buf[0] != NET_CMD_START)      return;
    if (buf[len - 1] != NET_CMD_END)  return;

    uint8_t src_dev = buf[1];
    uint8_t dst_dev = buf[2];
    uint8_t cmd     = buf[3];
    uint8_t arg0    = buf[4];
    uint8_t arg1    = buf[5];

    /* 只处理来自室内机（ID < OUTDOOR_1）的帧 */
    if (src_dev == 0 || src_dev >= DEVICE_OUTDOOR_1) return;

    uint8_t my_dev = get_local_dev();
    if (dst_dev != DEVICE_ALL && dst_dev != my_dev) return;

    switch ((NetCmdId)cmd) {

    /* 心跳（IdRepeatEvent 0x55）：偶数次发流状态，奇数次发版本 */
    case CMD_ID_REPEAT: {
        static int heart_count = 0;
        heart_count = (heart_count + 1) % 2;
        if (heart_count == 0)
            EventBusPublish(EVT_NET_HEARTBEAT_SEND, NULL, 0);
        else
            SvcNetworkVersionSend();
        break;
    }

    /* 远程开锁（UnlockEvent 0x54）*/
    case CMD_UNLOCK: {
        NetUnlockArg ua = {
            .unlock_time  = arg0,
            .lock_type    = (uint8_t)(arg1 & 0x03),
            .language_idx = (uint8_t)((arg1 & 0x3C) >> 2),
        };
        LOG_I("UNLOCK time=%ds type=%d lang=%d from=0x%02X",
              ua.unlock_time, ua.lock_type, ua.language_idx, src_dev);
        EventBusPublish(EVT_NET_UNLOCK_CMD, &ua, sizeof(ua));
        break;
    }

    /* 流控/监控保活（StreamStatusEvent 0x59）*/
    case CMD_STREAM_STATUS: {
        NetStreamStatus st = {
            .sender_dev       = src_dev,
            .key_frame_req    = (uint8_t)(!(arg0 & 0x01)),
            .leave_msg_enable = (uint8_t)((arg0 & 0x02) >> 1),
            .leave_msg_lang   = (uint8_t)(arg0 >> 2),
            .monitor_enable   = (uint8_t)((arg0 & 0x08) >> 3),
            .audio_volume     = (uint8_t)(arg1 * 3 + 66),
        };
        LOG_D("STREAM_STATUS kf=%d leave=%d(lang=%d) vol=%d",
              st.key_frame_req, st.leave_msg_enable, st.leave_msg_lang, st.audio_volume);
        EventBusPublish(EVT_NET_STREAM_STATUS, &st, sizeof(st));
        break;
    }

    /* 室内机接听（OutdoorTalkEvent 0x57）*/
    case CMD_OUTDOOR_TALK: {
        NetCallArg ca = { .sender_dev = src_dev, .channel = arg0 };
        LOG_I("OUTDOOR_TALK from=0x%02X ch=%d", src_dev, arg0);
        EventBusPublish(EVT_NET_CALL_START, &ca, sizeof(ca));
        break;
    }

    /* 挂断（OutdoorHangEvent 0x75）*/
    case CMD_OUTDOOR_HANG:
        LOG_I("OUTDOOR_HANG from=0x%02X", src_dev);
        EventBusPublish(EVT_NET_CALL_END, NULL, 0);
        break;

    /* 出厂复位（OutdoorResetEvent 0x65）*/
    case CMD_OUTDOOR_RESET:
        LOG_W("OUTDOOR_RESET from=0x%02X", src_dev);
        EventBusPublish(EVT_NET_RESET_CMD, NULL, 0);
        break;

    /* 固件升级（UpgraedOutdoorEvent 0x62）*/
    case CMD_UPGRADE: {
        NetUpgradeArg upg;
        memset(&upg, 0, sizeof(upg));
        upg.sender_dev   = src_dev;
        upg.is_long_pack = (len > NET_PACK_LEN) ? 1 : 0;

        if (upg.is_long_pack) {
            /* 长包最小长度：起始码(1) + src(1) + dst(1) + cmd(1) + index(4) + datalen(4) + sum(1) + end(1) = 14 */
            if (len < 14) break;
            upg.arg1 = (uint32_t)((buf[4]<<24)|(buf[5]<<16)|(buf[6]<<8)|buf[7]);
            upg.arg2 = (uint32_t)((buf[8]<<24)|(buf[9]<<16)|(buf[10]<<8)|buf[11]);
            upg.data = (uint8_t *)&buf[12];
            int avail = len - 12 - 2;  /* 去掉头部(12B) + 尾部 sum+end(2B) */
            if (avail < 0) avail = 0;
            upg.data_len = ((int)upg.arg2 > 0 && (int)upg.arg2 <= avail)
                           ? (int)upg.arg2 : avail;
            LOG_D("UPGRADE longpack index=%u datalen=%u", upg.arg1, upg.arg2);
        } else {
            upg.ctrl_arg0 = arg0;
            upg.ctrl_arg1 = arg1;
            LOG_I("UPGRADE ctrl arg0=0x%02X arg1=0x%02X from=0x%02X",
                  arg0, arg1, src_dev);
        }
        EventBusPublish(EVT_NET_UPGRADE_CMD, &upg, sizeof(upg));
        break;
    }

    /* 移动侦测灵敏度（MotionSensitivityEvent 0x63）*/
    case CMD_MOTION_SENS: {
        int sens = (int)arg0;
        LOG_I("MOTION_SENS=%d from=0x%02X", sens, src_dev);
        EventBusPublish(EVT_NET_MOTION_SENSITIVITY, &sens, sizeof(sens));
        break;
    }

    case CMD_ADD_DEL_CARD:
        LOG_D("ADD_DEL_CARD from=0x%02X arg0=%d arg1=%d", src_dev, arg0, arg1);
        break;

    default:
        break;
    }
}

/* =========================================================
 *  接收线程
 * ========================================================= */
static int recv_sock_create(void)
{
    int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_CMD));
    if (fd < 0) { LOG_E("recv socket create fail"); return -1; }

    if (NetRawPromiscuousSet(NET_IFACE) < 0) { close(fd); return -1; }
    if (NetRawIfrBind(fd, NET_IFACE, ETH_P_CMD) < 0) { close(fd); return -1; }

    return fd;
}

static void *net_recv_thread(void *arg)
{
    (void)arg;
    static uint8_t recv_buf[1024 + 73];
    int eth_reset = 1;

    int fd = recv_sock_create();
    if (fd < 0) {
        LOG_E("recv sock create fail, thread exit");
        return NULL;
    }

    pthread_mutex_lock(&s_net.state_lock);
    s_net.recv_fd = fd;
    pthread_mutex_unlock(&s_net.state_lock);

    LOG_I("recv thread start fd=%d", fd);

    while (1) {
        pthread_mutex_lock(&s_net.state_lock);
        int running = s_net.running;
        pthread_mutex_unlock(&s_net.state_lock);
        if (!running) break;

        memset(recv_buf, 0, sizeof(recv_buf));
        int recv_len = NetRawPacketReceive(fd, recv_buf, sizeof(recv_buf), 5000);

        if (recv_len > 0) {
            eth_reset = 0;
            raw_packet_handle(recv_buf, recv_len);
        } else if (recv_len == 0 && eth_reset == 0) {
            /* select 超时且之前曾收到过数据 → 复位网口 */
            eth_reset = 1;
            LOG_W("recv timeout, reset eth0");
            eth_interface_state(NET_IFACE, 0);
            usleep(1000);
            eth_interface_state(NET_IFACE, 1);
            continue;
        }

        usleep(1000);
    }

    pthread_mutex_lock(&s_net.state_lock);
    s_net.recv_fd = -1;
    pthread_mutex_unlock(&s_net.state_lock);
    close(fd);
    LOG_I("recv thread exit");
    return NULL;
}

static int send_sock_create(void)
{
    int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_CMD));
    if (fd < 0) { LOG_E("send socket create fail"); return -1; }
    return fd;
}

/* =========================================================
 *  公开接口
 * ========================================================= */
int SvcNetworkSend(const NetMsg *msg)
{
    if (!msg) return -1;
    net_msg_send(msg->device, msg->cmd, msg->arg1, msg->arg2);
    return 0;
}

void SvcNetworkDoorbellNotify(int key_index, int status)
{
    net_msg_send(DEVICE_ALL, CMD_DOORBELL,
                 (uint8_t)status, (uint8_t)(key_index + 1));
}

void SvcNetworkStreamStatusSend(uint8_t svp_active, uint8_t comm_active)
{
    uint8_t arg1 = (uint8_t)((svp_active & 0x01) | ((comm_active & 0x01) << 1));
    net_msg_send(DEVICE_ALL, CMD_STREAM_STATUS, arg1, DOOR_CAMERA_MODEL);
}

void SvcNetworkVersionSend(void)
{
    unsigned int ver = MAJOR_VER * 10000 + MINOR_VER * 100 + PATCH_VER;
    net_msg_send(DEVICE_ALL, CMD_COMPILE_TIME,
                 (uint8_t)(ver & 0xFF), (uint8_t)((ver >> 8) & 0xFF));
}

void SvcNetworkUpgradeReply(uint8_t dst_dev, uint8_t arg1, uint8_t arg2)
{
    net_msg_send(dst_dev, CMD_UPGRADE, arg1, arg2);
    LOG_I("upgrade reply → 0x%02X arg1=%d", dst_dev, arg1);
}

void SvcNetworkMotionDetectNotify(void)
{
    net_msg_send(DEVICE_ALL, CMD_MOTION_DETECT, 0, 0);
}

void SvcNetworkLocalDeviceSet(uint8_t dev_id)
{
    pthread_mutex_lock(&s_net.state_lock);
    s_net.local_dev = dev_id;
    pthread_mutex_unlock(&s_net.state_lock);
    LOG_I("local dev=0x%02X (%s)",
          dev_id, (dev_id == DEVICE_OUTDOOR_1) ? "DOOR1" : "DOOR2");
}

uint8_t SvcNetworkLocalDeviceGet(void)
{
    return get_local_dev();
}

int SvcNetworkInit(void)
{
    uint8_t local = get_local_dev();
    const char *ip = (local == DEVICE_OUTDOOR_1) ? "192.168.37.7" : "192.168.37.8";

    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ifconfig %s %s netmask 255.255.255.0", NET_IFACE, ip);
    system(cmd);
    LOG_I("ip=%s dev=0x%02X", ip, local);

    int sfd = send_sock_create();
    if (sfd < 0) return -1;

    struct sockaddr_ll sll;
    if (NetRawIfrAddrConfig(sfd, NET_IFACE, &sll) < 0) {
        close(sfd); return -1;
    }

    pthread_mutex_lock(&s_net.state_lock);
    s_net.send_fd  = sfd;
    s_net.send_sll = sll;
    s_net.running  = 1;
    pthread_mutex_unlock(&s_net.state_lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, net_recv_thread, NULL) != 0) {
        LOG_E("create recv thread fail");
        pthread_mutex_lock(&s_net.state_lock);
        s_net.running = 0;
        if (s_net.send_fd >= 0) { close(s_net.send_fd); s_net.send_fd = -1; }
        pthread_mutex_unlock(&s_net.state_lock);
        return -1;
    }
    pthread_detach(tid);

    LOG_I("init ok");
    return 0;
}

int SvcNetworkDeinit(void)
{
    pthread_mutex_lock(&s_net.state_lock);
    s_net.running = 0;
    if (s_net.send_fd >= 0) { close(s_net.send_fd); s_net.send_fd = -1; }
    pthread_mutex_unlock(&s_net.state_lock);
    return 0;
}
