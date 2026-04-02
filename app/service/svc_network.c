/**
 * @file    svc_network.c
 * @brief   命令通道网络服务
 *
 * ===================== 职责分离 =====================
 *    职责：命令帧打包/分发，设备 ID 管理，EventBus 发布，
 *          接收线程（含 eth0 异常复位），对外发送接口。
 *
 *  hal/drv_net_raw.c
 *    职责：Raw Socket 创建/绑定/混杂模式，MAC 头封装，
 *          帧发送（NetRawPacketSend），帧接收（NetRawPacketReceive）。
 *    本文件只调用 drv_net_raw 接口，不做任何 socket 细节操作。
 *
 * ===================== 协议=====================
 *
 *  帧格式（8字节短包）：
 *    [0xAA][src][dst][cmd][arg1][arg2][sum][0x55]
 *    sum = (src+dst+cmd+arg1+arg2) & 0xFF
 *
 *  长包（DataLen > 8，升级数据）：
 *    [0xAA][src][dst][0x62][DP[0..n]][sum][0x55]
 *    DP[0..3] = 包序号, DP[4..7] = 数据长度, DP[8..] = 数据
 *
 *  命令 ID：
 *    0x53 LightEvent
 *    0x54 UnlockEvent           远程开锁
 *    0x55 IdRepeatEvent         心跳
 *    0x56 DoorbellEvent         门铃
 *    0x57 OutdoorTalkEvent      室内机接听
 *    0x58 IntercomCallEvent
 *    0x59 StreamStatusEvent     流控/监控保活
 *    0x60 DeviceBusyEvent
 *    0x61 MotionDelectEvent
 *    0x62 UpgraedOutdoorEvent   固件升级
 *    0x63 MotionSensitivityEvent
 *    0x64 AddDelCardEvent       网络加删卡
 *    0x65 OutdoorResetEvent     出厂复位
 *    0x66 MailboxStatusEvent
 *    0x70 CompileTimeEvent      版本信息
 *    0x71 DefaultUnlockTimeEvent
 *    0x72 ExitButtonTimeEvent
 *    0x75 OutdoorHangEvent      挂断
 *    0x99 Gate2UnlockEvent
 *
 *  设备 ID（NetworkDevice 枚举）：
 *    1~6  DEVICE_INDOOR_ID1~6
 *    7    DEVICE_OUTDOOR_1
 *    8    DEVICE_OUTDOOR_2
 *    0xFF DEVICE_ALL
 *
 *  接收过滤（RawPacketHandle）：
 *    buf[1] < DEVICE_OUTDOOR_1  → 只处理来自室内机的帧
 *    buf[3] < TotalEvent        → cmd 在枚举范围内
 *    buf[2] == DEVICE_ALL 或 本机 ID
 */
#include "svc_network.h"
#include "drv_net_raw.h"        /* hal 层：所有 Raw Socket 操作 */
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
#define NET_IFACE          "eth0"     /* NETWORK_INTERFACE_NAME */
#define ETH_P_CMD          0xFFFF     /* 自定义以太网协议号 */

#define NET_CMD_START      0xAA       /* NET_COMMON_CMD_START */
#define NET_CMD_END        0x55       /* NET_COMMON_CMD_END   */
#define NET_PACK_LEN       8          /* 标准短包长度 */

/* 设备 ID（NetworkDevice 枚举值）*/
#define DEVICE_OUTDOOR_1   7
#define DEVICE_OUTDOOR_2   8
#define DEVICE_ALL         0xFF

/* 命令 ID */
typedef enum {
    CMD_LIGHT_EVENT    = 0x53,
    CMD_UNLOCK         = 0x54,   /* UnlockEvent           */
    CMD_ID_REPEAT      = 0x55,   /* IdRepeatEvent（心跳）  */
    CMD_DOORBELL       = 0x56,   /* DoorbellEvent          */
    CMD_OUTDOOR_TALK   = 0x57,   /* OutdoorTalkEvent       */
    CMD_STREAM_STATUS  = 0x59,   /* StreamStatusEvent      */
    CMD_DEVICE_BUSY    = 0x60,
    CMD_MOTION_DETECT  = 0x61,
    CMD_UPGRADE        = 0x62,   /* UpgraedOutdoorEvent    */
    CMD_MOTION_SENS    = 0x63,   /* MotionSensitivityEvent */
    CMD_ADD_DEL_CARD   = 0x64,   /* AddDelCardEvent        */
    CMD_OUTDOOR_RESET  = 0x65,   /* OutdoorResetEvent      */
    CMD_COMPILE_TIME   = 0x70,   /* CompileTimeEvent       */
    CMD_EXIT_BTN_TIME  = 0x72,   /* ExitButtonTimeEvent    */
    CMD_OUTDOOR_HANG   = 0x75,   /* OutdoorHangEvent       */
    CMD_GATE2_UNLOCK   = 0x99,   /* Gate2UnlockEvent       */
} NetCmdId;

#ifndef MAJOR_VER
#define MAJOR_VER 1
#define MINOR_VER 0
#define PATCH_VER 0
#endif
#ifndef DOOR_CAMERA_MODEL
#define DOOR_CAMERA_MODEL 0
#endif

/* =========================================================
 *  模块状态结构体
 * ========================================================= */
typedef struct {
    pthread_mutex_t send_mutex;       /* 发送锁*/
    pthread_mutex_t state_lock;       /* 状态读写锁 */
    int             send_fd;          /* 发送 socket*/
    int             recv_fd;          /* 接收 socket（由接收线程管理）*/
    struct sockaddr_ll send_sll;      /* 发送地址*/
    uint8_t         local_dev;        /* 本机设备ID*/
    int             running;          /* 接收线程运行标志 */
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
 *  NetworkCodePack
 *  dst[6] = (src+dst+cmd+arg1+arg2) & 0xFF
 * ========================================================= */
static void net_code_pack(uint8_t dst_dev, uint8_t cmd,
                           uint8_t arg1,   uint8_t arg2,
                           uint8_t *out)
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

/* =========================================================
 *  NetworkMsgSned 封装
 *  打包 + 加发送锁 + 调 drv_net_raw::NetRawPacketSend
 * ========================================================= */
static void net_msg_send(uint8_t dst, uint8_t cmd, uint8_t arg1, uint8_t arg2)
{
    uint8_t buf[NET_PACK_LEN];
    net_code_pack(dst, cmd, arg1, arg2, buf);

    pthread_mutex_lock(&s_net.send_mutex);   
    pthread_mutex_lock(&s_net.state_lock);
    int fd = s_net.send_fd;
    struct sockaddr_ll sll = s_net.send_sll;
    pthread_mutex_unlock(&s_net.state_lock);

    if (fd >= 0) {
        NetRawPacketSend(fd, &sll, buf, NET_PACK_LEN, NET_IFACE, ETH_P_CMD);
    }
    pthread_mutex_unlock(&s_net.send_mutex);
}

/* =========================================================
 *  EthInterfaceState
 *  因为它是"网口异常复位"的业务决策，不属于纯 HAL 操作）
 * ========================================================= */
static int eth_interface_state(const char *iface, int enable)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("Socket creation failed"); return -1; }

    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        perror("ioctl(SIOCGIFFLAGS) failed"); close(fd); return -1;
    }
    if (enable) ifr.ifr_flags |=  IFF_UP;
    else        ifr.ifr_flags &= (short)~IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        perror("ioctl(SIOCSIFFLAGS) failed"); close(fd); return -1;
    }
    printf("Network interface %s has been %s\n", iface, enable ? "enabled" : "disabled");
    close(fd);
    return 0;
}

/* =========================================================
 *  RawPacketHandle
 *    buf[0] == NET_COMMON_CMD_START
 *    buf[len-1] == NET_COMMON_CMD_END
 *    buf[1] < DEVICE_OUTDOOR_1       ← 来自室内机
 *    buf[3] < TotalEvent             ← 有效 cmd
 *    HandleFuncGroup[cmd].proc != NULL
 *    ReceiveDev == DEVICE_ALL 或 LocalDeviceId
 * ========================================================= */
static void raw_packet_handle(const uint8_t *buf, int len)
{
    if (len < NET_PACK_LEN) return;
    if (buf[0] != NET_CMD_START) return;
    if (buf[len - 1] != NET_CMD_END) return;

    uint8_t src_dev = buf[1];
    uint8_t dst_dev = buf[2];
    uint8_t cmd     = buf[3];
    uint8_t arg0    = buf[4];
    uint8_t arg1    = buf[5];
    /* buf[6] = sum，不验证*/

    /* buf[1] < DEVICE_OUTDOOR_1(7)：只处理来自室内机的帧 */
    if (src_dev == 0 || src_dev >= DEVICE_OUTDOOR_1) return;

    /* ReceiveDev 过滤 */
    uint8_t my_dev = get_local_dev();
    if (dst_dev != DEVICE_ALL && dst_dev != my_dev) return;

    /* ---- 命令分发---- */
    switch ((NetCmdId)cmd) {

    /* ---- IdRepeatEvent (0x55)：心跳 ----
     *   每 1s 处理一次（DiffClockTimeMs > 1000）
     *   HeartCount 0→StreamStatusEvent 1→CompileTimeEvent 交替
     */
    case CMD_ID_REPEAT: {
        static int heart_count = 0;
        static struct timespec last_hb = {0};
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long diff_ms = (long)(now.tv_sec  - last_hb.tv_sec)  * 1000
                     + (long)(now.tv_nsec - last_hb.tv_nsec) / 1000000;
        if (diff_ms < 1000) break;
        last_hb = now;

        heart_count = (heart_count + 1) % 2;
        if (heart_count == 0) {
            /* case 0：让 app_intercom 发 StreamStatusEvent 回包 */
            EventBusPublish(EVT_NET_HEARTBEAT_SEND, NULL, 0);
        } else {
            /* case 1：发 CompileTimeEvent 版本信息 */
            SvcNetworkVersionSend();
        }
        break;
    }

    /* ---- UnlockEvent (0x54)：远程开锁 ----
     *   UNLOCK_TIME = Arg[0]
     *   UNLOCK_TYPE = Arg[1] & 0x03
     *   语言索引    = (Arg[1] & 0x3C) >> 2
     */
    case CMD_UNLOCK: {
        NetUnlockArg ua = {
            .unlock_time  = arg0,
            .lock_type    = (uint8_t)(arg1 & 0x03),
            .language_idx = (uint8_t)((arg1 & 0x3C) >> 2),
        };
        printf("[SvcNet] UNLOCK time=%ds type=%d lang=%d from=0x%02X\n",
               ua.unlock_time, ua.lock_type, ua.language_idx, src_dev);
        EventBusPublish(EVT_NET_UNLOCK_CMD, &ua, sizeof(ua));
        break;
    }

    /* ---- StreamStatusEvent (0x59)：流控/监控保活 ----
     *   KEY_FRAME_REQUEST    = !(Arg[0] & 0x01)
     *   LEAVE_MESSAGE_ENABLE = Arg[0] & 0x02
     *   LEAVE_MSG_VOICE_INDEX= LeaveMsgEng + (Arg[0] >> 2)
     *   TUYA_MONIOTR_ENABLE  = Arg[0] & 0x08
     *   AUDIO_TALK_VOLUME    = Arg[1] * 3 + 66
     */
    case CMD_STREAM_STATUS: {
        NetStreamStatus st = {
            .sender_dev       = src_dev,
            .key_frame_req    = (uint8_t)(!(arg0 & 0x01)),
            .leave_msg_enable = (uint8_t)((arg0 & 0x02) >> 1),
            .leave_msg_lang   = (uint8_t)(arg0 >> 2),    
            .monitor_enable   = (uint8_t)((arg0 & 0x08) >> 3),
            .audio_volume     = (uint8_t)((arg1) * 3 + 66),
        };
        printf("[SvcNet] STREAM_STATUS kf=%d leave=%d(lang=%d) monitor=%d vol=%d\n",
               st.key_frame_req, st.leave_msg_enable, st.leave_msg_lang,
               st.monitor_enable, st.audio_volume);
        EventBusPublish(EVT_NET_STREAM_STATUS, &st, sizeof(st));
        break;
    }

    /* ---- OutdoorTalkEvent (0x57)：室内机接听 ----
     *   CommCh = Arg[0]
     *   if (!TimerEnablestatus(CommunicateTimer) &&
     *       (DEVICE_OUTDOOR_1 + CommCh - 1) == NetLocalDeviceIDGet())
     *   → 进入通话
     */
    case CMD_OUTDOOR_TALK: {
        NetCallArg ca = { .sender_dev = src_dev, .channel = arg0 };
        printf("[SvcNet] OUTDOOR_TALK from=0x%02X ch=%d\n", src_dev, arg0);
        EventBusPublish(EVT_NET_CALL_START, &ca, sizeof(ca));
        break;
    }

    /* ---- OutdoorHangEvent (0x75)：挂断 ----
     */
    case CMD_OUTDOOR_HANG:
        printf("[SvcNet] OUTDOOR_HANG from=0x%02X\n", src_dev);
        EventBusPublish(EVT_NET_CALL_END, NULL, 0);
        break;

    /* ---- OutdoorResetEvent (0x65)：恢复出厂 ----
     */
    case CMD_OUTDOOR_RESET:
        printf("[SvcNet] OUTDOOR_RESET from=0x%02X\n", src_dev);
        EventBusPublish(EVT_NET_RESET_CMD, NULL, 0);
        break;

    /* ---- UpgraedOutdoorEvent (0x62)：固件升级 ----
     * UpgradeLongPack = (DataLen > 8)
     *   长包：
     *     DP = &buf[4]
     *     arg1_val = DP[0]<<24|DP[1]<<16|DP[2]<<8|DP[3]  ← 包序号
     *     arg2_val = DP[4]<<24|DP[5]<<16|DP[6]<<8|DP[7]  ← 数据长度
     *     data     = &DP[8]
     *     if (ReceiveUpgradePack(arg1,arg2,data)==0) → 发 Arg1=3
     *
     * 短包：
     *   CheckOnlineStatus = Arg[0] & 0x01 → 应答 Arg1=1 Arg2=1
     *   UpdateOver        = Arg[0] & 0x02
     *     UpdataFinish    = Arg[1] & 0x01 → sleep(2) → 升级 → 应答 Arg1=2
     *     UpdataFail      = Arg[1] & 0x02 → 取消
     */
    case CMD_UPGRADE: {
        NetUpgradeArg upg;
        memset(&upg, 0, sizeof(upg));
        upg.sender_dev   = src_dev;
        upg.is_long_pack = (len > NET_PACK_LEN) ? 1 : 0;

        if (upg.is_long_pack) {
            /* 长包：buf[4..] = DP[]
             * DP[0..3]=index, DP[4..7]=data_len, DP[8..]=data */
            if (len < (NET_PACK_LEN + 8)) break;  /* 最小：header(4)+index(4)+len(4)+sum+end */
            upg.arg1 = (uint32_t)((buf[4]<<24)|(buf[5]<<16)|(buf[6]<<8)|buf[7]);
            upg.arg2 = (uint32_t)((buf[8]<<24)|(buf[9]<<16)|(buf[10]<<8)|buf[11]);
            upg.data = (uint8_t *)&buf[12];
            /* 有效数据长度 = arg2，不得超出接收缓冲（去尾部 sum+end 共2字节）*/
            int avail = len - 12 - 2;
            if (avail < 0) avail = 0;
            upg.data_len = ((int)upg.arg2 > 0 && (int)upg.arg2 <= avail)
                           ? (int)upg.arg2 : avail;
            printf("[SvcNet] UPGRADE LONGPACK index=%u datalen=%u avail=%d\n",
                   upg.arg1, upg.arg2, avail);
        } else {
            upg.ctrl_arg0 = arg0;
            upg.ctrl_arg1 = arg1;
            printf("[SvcNet] UPGRADE CTRL arg0=0x%02X(%s) arg1=0x%02X from=0x%02X\n",
                   arg0,
                   (arg0 & 0x01) ? "QueryOnline" :
                   (arg0 & 0x02) ? ((arg1 & 0x01) ? "UpdataFinish" :
                                    (arg1 & 0x02) ? "UpdataFail"   : "UpdateOver") : "?",
                   arg1, src_dev);
        }
        EventBusPublish(EVT_NET_UPGRADE_CMD, &upg, sizeof(upg));
        break;
    }

    /* ---- MotionSensitivityEvent (0x63)：移动侦测灵敏度 ----
     *   SvpMdFiltersSet(SvpSensit[Sensitivity][0], SvpSensit[Sensitivity][1])
     */
    case CMD_MOTION_SENS: {
        int sens = (int)arg0;
        printf("[SvcNet] MOTION_SENS=%d from=0x%02X\n", sens, src_dev);
        EventBusPublish(EVT_NET_MOTION_SENSITIVITY, &sens, sizeof(sens));
        break;
    }

    /* ---- AddDelCardEvent (0x64)：网络加删卡 ---- */
    case CMD_ADD_DEL_CARD:
        printf("[SvcNet] ADD_DEL_CARD from=0x%02X arg0=%d arg1=%d\n",
               src_dev, arg0, arg1);
        /* TODO: 发布专用事件给 app_net_manage */
        break;

    default:
        /* 忽略*/
        break;
    }
}

/* =========================================================
 *  创建接收 socket，设混杂模式，绑定网卡
 *  调用 drv_net_raw： NetRawIfrBind
 * ========================================================= */
static int recv_sock_create(void)
{
    int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_CMD));
    if (fd < 0) { perror("[SvcNet] recv socket create failed"); return -1; }

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
        printf("[SvcNet] recv sock create fail, thread exit\n");
        return NULL;
    }

    pthread_mutex_lock(&s_net.state_lock);
    s_net.recv_fd = fd;
    pthread_mutex_unlock(&s_net.state_lock);

    printf("[SvcNet] recv thread start fd=%d\n", fd);

    while (1) {
        pthread_mutex_lock(&s_net.state_lock);
        int running = s_net.running;
        pthread_mutex_unlock(&s_net.state_lock);
        if (!running) break;

        memset(recv_buf, 0, sizeof(recv_buf));

        /* NetworkCmdReceive = RawPacketReceive(NetRecvFd, buf, size, 5000)
         * drv_net_raw 内部已完成 MAC 头剥除*/
        int recv_len = NetRawPacketReceive(fd, recv_buf, sizeof(recv_buf), 5000);

        if (recv_len > 0) {
            /* 成功收到数据 */
            eth_reset = 0;
            raw_packet_handle(recv_buf, recv_len);
        } else if (recv_len == 0 && eth_reset == 0) {
            /* select 超时（0），且之前曾收到过数据 → 复位网口*/
            eth_reset = 1;
            printf("[SvcNet] Raw Socket Receive Anomaly!!!\n");
            eth_interface_state(NET_IFACE, 0);
            usleep(1000);
            eth_interface_state(NET_IFACE, 1);
            continue;
        }
        /* recv_len < 0（错误）或 recv_len==0 且 eth_reset==1 → 继续等待 */

        usleep(1000);  
    }

    /* exit 标签：关闭 socket*/
    pthread_mutex_lock(&s_net.state_lock);
    s_net.recv_fd = -1;
    pthread_mutex_unlock(&s_net.state_lock);
    close(fd);
    printf("============ [SvcNet] [exit] recv thread ============\n");
    return NULL;
}

static int send_sock_create(void)
{
    int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_CMD));
    if (fd < 0) { perror("[SvcNet] send socket create failed"); return -1; }
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

/**
 * @brief 门铃通知（DoorbellEvent 0x56）
 * Data.Cmd=DoorbellEvent, Data.Arg1=status, Data.Arg2=key+1
 */
void SvcNetworkDoorbellNotify(int key_index, int status)
{
    net_msg_send(DEVICE_ALL, CMD_DOORBELL,
                 (uint8_t)status, (uint8_t)(key_index + 1));
}

/**
 * @brief 流状态应答（StreamStatusEvent 0x59，回复心跳 case 0）
 *   Arg1 = (0<<2) | TimerEnablestatus(SVPTimer) | (TimerEnablestatus(CommunicateTimer)<<1)
 *   Arg2 = DOOR_CAMERA_MODEL
 */
void SvcNetworkStreamStatusSend(uint8_t svp_active, uint8_t comm_active)
{
    uint8_t arg1 = (uint8_t)((0 << 2)
                             | (svp_active  & 0x01)
                             | ((comm_active & 0x01) << 1));
    net_msg_send(DEVICE_ALL, CMD_STREAM_STATUS, arg1, DOOR_CAMERA_MODEL);
}

/**
 * @brief 版本信息（CompileTimeEvent 0x70，回复心跳 case 1）
 *   ver = MAJOR*10000 + MINOR*100 + PATCH
 *   Arg1 = ver & 0xFF
 *   Arg2 = (ver >> 8) & 0xFF
 */
void SvcNetworkVersionSend(void)
{
    unsigned int ver = MAJOR_VER * 10000u + MINOR_VER * 100u + PATCH_VER;
    net_msg_send(DEVICE_ALL, CMD_COMPILE_TIME,
                 (uint8_t)(ver & 0xFF), (uint8_t)((ver >> 8) & 0xFF));
}

/**
 * @brief 升级应答（UpgraedOutdoorEvent 0x62）
 * arg1: 1=在线确认  2=开始执行  3=升级失败
 * arg2: 固定 1
 */
void SvcNetworkUpgradeReply(uint8_t dst_dev, uint8_t arg1, uint8_t arg2)
{
    net_msg_send(dst_dev, CMD_UPGRADE, arg1, arg2);
    printf("[SvcNet] upgrade reply → 0x%02X Arg1=%d Arg2=%d\n",
           dst_dev, arg1, arg2);
}

/**
 * @brief 移动侦测通知（MotionDelectEvent CMD_MOTION_DETECT 0x61）
 *   SVP 检测到人形/运动时向所有室内机广播，室内机据此触发 APP 推送等。
 *   对应旧版 SmartVisionPlatformCallback 触发后由上层发送的通知包。
 */
void SvcNetworkMotionDetectNotify(void)
{
    net_msg_send(DEVICE_ALL, CMD_MOTION_DETECT, 0, 0);
}

void SvcNetworkLocalDeviceSet(uint8_t dev_id)
{
    pthread_mutex_lock(&s_net.state_lock);
    s_net.local_dev = dev_id;
    pthread_mutex_unlock(&s_net.state_lock);
    printf("[SvcNet] local dev = 0x%02X (%s)\n",
           dev_id, (dev_id == DEVICE_OUTDOOR_1) ? "DOOR1" : "DOOR2");
}

uint8_t SvcNetworkLocalDeviceGet(void)
{
    return get_local_dev();
}

/* =========================================================
 *  SvcNetworkInit
 *  分层调用：
 *    recv_sock_create → NetRawPromiscuousSet + NetRawIfrBind（drv_net_raw）
 *    send_sock_create → socket()
 *    RawNetIfrAddrConfig → NetRawIfrAddrConfig（drv_net_raw）
 *    NetworkMsgSned → net_msg_send → NetRawPacketSend（drv_net_raw）
 * ========================================================= */
int SvcNetworkInit(void)
{
    uint8_t local = get_local_dev();
    const char *ip = (local == DEVICE_OUTDOOR_1) ? "192.168.37.7" : "192.168.37.8";

    /* ifconfig eth0 IP netmask 255.255.255.0 */
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ifconfig %s %s netmask 255.255.255.0", NET_IFACE, ip);
    system(cmd);
    printf("[SvcNet] ip=%s dev=0x%02X\n", ip, local);

    /* NetMsgSendSockCreate */
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

    /* NetMsgReceiveTaskCreate（接收线程内部调用 recv_sock_create）*/
    pthread_t tid;
    if (pthread_create(&tid, NULL, net_recv_thread, NULL) != 0) {
        perror("[SvcNet] pthread_create"); return -1;
    }
    pthread_detach(tid);

    printf("[SvcNet] init ok\n");
    return 0;
}

int SvcNetworkDeinit(void)
{
    pthread_mutex_lock(&s_net.state_lock);
    s_net.running = 0;
    if (s_net.send_fd >= 0) { close(s_net.send_fd); s_net.send_fd = -1; }
    /* s_net.recv_fd 由接收线程在退出时关闭 */
    pthread_mutex_unlock(&s_net.state_lock);
    return 0;
}
