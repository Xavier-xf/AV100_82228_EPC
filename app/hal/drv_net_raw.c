/**
 * @file    drv_net_raw.c
 * @brief   链路层原始套接字工具库
 */
#define LOG_TAG "NetRaw"
#include "log.h"

#include "drv_net_raw.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define ETH_HEADER_LEN        14
#define ETH_FRAME_MAX_NO_FCS 1514
#define RAW_PREFIX_LEN        60
#define NET_RAW_MAX_PAYLOAD  (ETH_FRAME_MAX_NO_FCS - RAW_PREFIX_LEN)
#define NET_RAW_RECV_BUF_LEN 2048

int NetRawPromiscuousSet(const char *iface)
{
    int fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { LOG_E("socket fail"); return -1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        LOG_E("SIOCGIFFLAGS fail"); close(fd); return -1;
    }
    ifr.ifr_flags |= IFF_PROMISC;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        LOG_E("SIOCSIFFLAGS fail"); close(fd); return -1;
    }
    close(fd);
    return 0;
}

int NetRawMacGet(const char *iface, char *mac_out)
{
    int fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { LOG_E("socket fail"); return -1; }

    struct ifreq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.ifr_name, iface, IFNAMSIZ - 1);
    req.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(fd, SIOCGIFHWADDR, &req) < 0) {
        LOG_E("SIOCGIFHWADDR fail"); close(fd); return -1;
    }
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
        LOG_E("bind fail"); return -1;
    }
    return 0;
}

int NetRawIfrAddrConfig(int fd, const char *iface, struct sockaddr_ll *sll)
{
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.ifr_name, iface, IFNAMSIZ - 1);
    req.ifr_name[IFNAMSIZ - 1] = '\0';
    if (ioctl(fd, SIOCGIFINDEX, &req) < 0) {
        LOG_E("SIOCGIFINDEX fail"); return -1;
    }
    memset(sll, 0, sizeof(*sll));
    sll->sll_ifindex = req.ifr_ifindex;
    return 0;
}

int NetRawPacketHead(uint8_t *buf, const char *iface, int protocol)
{
    static char *prefix = NULL;

    if (!prefix) {
        prefix = calloc(1, RAW_PREFIX_LEN);
        if (!prefix) return -1;

        /* Destination MAC is fixed to 01:01:01:01:01:01. */
        prefix[0] = prefix[1] = prefix[2] = 0x01;
        prefix[3] = prefix[4] = prefix[5] = 0x01;
        if (NetRawMacGet(iface, &prefix[6]) < 0) {
            free(prefix);
            prefix = NULL;
            return -1;
        }
    }

    /* Bytes [0..13] are the real Ethernet header.
     * Bytes [14..59] are legacy zero padding kept for compatibility.
     */
    prefix[12] = (char)(protocol / 256);
    prefix[13] = (char)(protocol % 256);
    memcpy(buf, prefix, RAW_PREFIX_LEN);
    return 0;
}

int NetRawPacketSend(int fd, struct sockaddr_ll *sll,
                     const uint8_t *data, int size,
                     const char *iface, int protocol)
{
    uint8_t pkg[ETH_FRAME_MAX_NO_FCS];
    int sent = 0;

    while (size > 0) {
        int chunk = (size > NET_RAW_MAX_PAYLOAD) ? NET_RAW_MAX_PAYLOAD : size;
        memset(pkg, 0, sizeof(pkg));
        if (NetRawPacketHead(pkg, iface, protocol) < 0) return -1;
        memcpy(&pkg[RAW_PREFIX_LEN], &data[sent], (size_t)chunk);
        if (sendto(fd, pkg, (size_t)(chunk + RAW_PREFIX_LEN), 0,
                   (struct sockaddr *)sll, sizeof(*sll)) < 0) {
            LOG_E("sendto fail"); return -1;
        }
        sent += chunk;
        size -= chunk;
    }
    return 0;
}

int NetRawPacketReceive(int fd, uint8_t *buf, int size, unsigned int timeout_ms)
{
    /* This helper returns only payload bytes.
     * The incoming frame is expected to carry a fixed 60-byte prefix:
     *   [0..13]  real Ethernet header
     *   [14..59] reserved/padding bytes kept by the legacy protocol
     */
    uint8_t rbuf[NET_RAW_RECV_BUF_LEN];
    int recv_max = (int)sizeof(rbuf);

    fd_set fds;
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return ret;

    int n = (int)recvfrom(fd, rbuf, (size_t)recv_max, 0, NULL, NULL);
    if (n <= RAW_PREFIX_LEN) return -1;

    int payload = n - RAW_PREFIX_LEN;
    if (payload > size) payload = size;
    memcpy(buf, &rbuf[RAW_PREFIX_LEN], (size_t)payload);
    return payload;
}

int NetRawFrameReceive(int fd, uint8_t *buf, int size, unsigned int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    int ret = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) return ret;

    int n = (int)recvfrom(fd, buf, (size_t)size, 0, NULL, NULL);
    return (n > 0) ? n : -1;
}
