/**
 * @file    drv_net_raw.h
 * @brief   链路层原始套接字工具库
 *
 * 供 svc_network / svc_intercom_stream 调用，消除重复的 Raw Socket 代码。
 * 不含任何业务逻辑，纯工具函数。
 */
#ifndef _DRV_NET_RAW_H_
#define _DRV_NET_RAW_H_

#include <netpacket/packet.h>
#include <net/if.h>
#include <stdint.h>

/** 设置网卡混杂模式 */
int  NetRawPromiscuousSet(const char *iface);

/** 获取网卡 MAC 地址（6字节）*/
int  NetRawMacGet(const char *iface, char *mac_out);

/** 绑定 Raw Socket 到网卡 */
int  NetRawIfrBind(int fd, const char *iface, int protocol);

/** 配置发送用的 sockaddr_ll（获取 ifindex，填写广播 MAC）*/
int  NetRawIfrAddrConfig(int fd, const char *iface, struct sockaddr_ll *sll);

/**
 * @brief 封装链路层数据头（60字节 MAC 头 + 2字节协议类型）
 *        目的 MAC 固定为 01:01:01:01:01:01
 *        结果写入 buf[0..61]
 */
int  NetRawPacketHead(uint8_t *buf, const char *iface, int protocol);

/**
 * @brief 分包发送（按 MTU=1500 切片），自动填充 MAC 头
 */
int  NetRawPacketSend(int fd, struct sockaddr_ll *sll,
                      const uint8_t *data, int size,
                      const char *iface, int protocol);

/**
 * @brief 带超时的 Raw Socket 接收（返回有效负载长度，-1=超时/错误）
 *        已自动剥除 60 字节 MAC 头
 */
int  NetRawPacketReceive(int fd, uint8_t *buf, int size,
                          unsigned int timeout_ms);

/**
 * @brief 带超时的 Raw Socket 接收（保留完整以太帧，包含 MAC 头）
 *        返回完整帧长度，-1=超时/错误，0=无数据
 *        供 svc_network.c 使用，由调用方自行处理 MAC 头偏移
 */
int  NetRawFrameReceive(int fd, uint8_t *buf, int size,
                         unsigned int timeout_ms);

#endif /* _DRV_NET_RAW_H_ */
