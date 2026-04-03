/**
 * @file    drv_net_raw.c
 * @brief   链路层原始套接字工具库
 */
#include "drv_net_raw.h"
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAC_HEAD_LEN  60
#define MTU_LEN       1500

int NetRawPromiscuousSet(const char *iface)
{
    int fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[NetRaw] socket"); return -1; }

    struct ifreq ifr;
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) { perror("[NetRaw] SIOCGIFFLAGS"); close(fd); return -1; }
    ifr.ifr_flags |= IFF_PROMISC;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) { perror("[NetRaw] SIOCSIFFLAGS"); close(fd); return -1; }
    close(fd);
    return 0;
}

int NetRawMacGet(const char *iface, char *mac_out)
{
    int fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[NetRaw] socket"); return -1; }
    struct ifreq req;
    strncpy(req.ifr_name, iface, IFNAMSIZ - 1);
    ioctl(fd, SIOCGIFHWADDR, &req);
    close(fd);
    memcpy(mac_out, req.ifr_hwaddr.sa_data, 6);
    return 0;
}

int NetRawIfrBind(int fd, const char *iface, int protocol)
{
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family   = AF_PACKET;
    addr.sll_protocol = htons((uint16_t)protocol);
    addr.sll_ifindex  = (int)if_nametoindex(iface);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[NetRaw] bind"); return -1;
    }
    return 0;
}

int NetRawIfrAddrConfig(int fd, const char *iface, struct sockaddr_ll *sll)
{
    struct ifreq req;
    strncpy(req.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &req) < 0) { perror("[NetRaw] SIOCGIFINDEX"); return -1; }
    memset(sll, 0, sizeof(*sll));
    sll->sll_ifindex = req.ifr_ifindex;
    return 0;
}

int NetRawPacketHead(uint8_t *buf, const char *iface, int protocol)
{
    static char *mac_head = NULL;
    if (!mac_head) {
        mac_head = calloc(1, MAC_HEAD_LEN);
        if (!mac_head) return -1;
        /* 目的 MAC：01:01:01:01:01:01*/
        mac_head[0] = mac_head[1] = mac_head[2] = 0x01;
        mac_head[3] = mac_head[4] = mac_head[5] = 0x01;
        if (NetRawMacGet(iface, &mac_head[6]) < 0) { free(mac_head); mac_head = NULL; return -1; }
    }
    mac_head[12] = (char)(protocol / 256);
    mac_head[13] = (char)(protocol % 256);
    memcpy(buf, mac_head, MAC_HEAD_LEN);
    return 0;
}

int NetRawPacketSend(int fd, struct sockaddr_ll *sll,
                     const uint8_t *data, int size,
                     const char *iface, int protocol)
{
    uint8_t pkg[1514];
    int sent = 0;
    while (size > 0) {
        int chunk = (size > MTU_LEN) ? MTU_LEN : size;
        memset(pkg, 0, sizeof(pkg));
        NetRawPacketHead(pkg, iface, protocol);
        memcpy(&pkg[MAC_HEAD_LEN], &data[sent], chunk);
        if (sendto(fd, pkg, chunk + MAC_HEAD_LEN, 0,
                   (struct sockaddr *)sll, sizeof(*sll)) < 0) {
            perror("[NetRaw] sendto"); return -1;
        }
        sent += chunk;
        size -= chunk;
    }
    return 0;
}

int NetRawPacketReceive(int fd, uint8_t *buf, int size, unsigned int timeout_ms)
{
    /* 不使用 static 缓冲：audio_rx_thread 与 net_recv_thread 并发调用此函数，
     * static 共享缓冲会导致数据竞争，引起命令包丢失/损坏。
     * 改用栈上固定大小缓冲，以太帧最大 1514 字节 + 60 字节 MAC 头 = 1574 字节，
     * 取 2048 保留余量，满足所有调用场合。*/
    uint8_t rbuf[MAC_HEAD_LEN + 2048];
    int recv_max = (int)sizeof(rbuf);

    fd_set fds;
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    FD_ZERO(&fds); FD_SET(fd, &fds);

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return ret;

    int n = (int)recvfrom(fd, rbuf, (size_t)recv_max, 0, NULL, NULL);
    if (n <= MAC_HEAD_LEN) return -1;

    int payload = n - MAC_HEAD_LEN;
    if (payload > size) payload = size;
    memcpy(buf, &rbuf[MAC_HEAD_LEN], (size_t)payload);
    return payload;
}

/**
 * @brief 接收完整以太帧（含 MAC 头）
 * 返回帧总长度（含 MAC 头），0=超时，-1=错误
 */
int NetRawFrameReceive(int fd, uint8_t *buf, int size, unsigned int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    FD_ZERO(&fds); FD_SET(fd, &fds);

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return ret;   /* 0=超时，-1=错误 */

    int n = (int)recvfrom(fd, buf, (size_t)size, 0, NULL, NULL);
    return (n > 0) ? n : -1;
}
