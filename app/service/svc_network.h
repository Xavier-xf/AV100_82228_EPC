/**
 * @file    svc_network.h
 * @brief   命令通道网络服务接口
 */
#ifndef _SVC_NETWORK_H_
#define _SVC_NETWORK_H_

#include <stdint.h>

/* =========================================================
 *  通用消息结构（供 SvcNetworkSend 直接使用）
 * ========================================================= */
typedef struct {
    uint8_t device;   /* 目标设备 ID */
    uint8_t cmd;      /* 命令 ID（使用协议绝对值，如 0x56 = CMD_DOORBELL）*/
    uint8_t arg1;
    uint8_t arg2;
} NetMsg;

/* =========================================================
 *  发送接口
 * ========================================================= */

/** @brief 发送任意网络消息（低级接口）*/
int     SvcNetworkSend(const NetMsg *msg);

/** @brief 门铃按下通知室内机（DoorbellEvent 0x56）*/
void    SvcNetworkDoorbellNotify(int key_index, int status);

/** @brief 流状态应答（回复心跳 case 0，StreamStatusEvent 0x59）*/
void    SvcNetworkStreamStatusSend(uint8_t svp_active, uint8_t comm_active);

/** @brief 版本信息（回复心跳 case 1，CompileTimeEvent 0x70）*/
void    SvcNetworkVersionSend(void);

/** @brief 升级应答（UpgraedOutdoorEvent 0x62，arg1: 1=在线 2=开始 3=失败）*/
void    SvcNetworkUpgradeReply(uint8_t dst_dev, uint8_t arg1, uint8_t arg2);

/** @brief 移动侦测通知室内机（MotionDelectEvent 0x61）
 *   SVP 检测到人形/运动时调用，通知所有室内机触发推送等处理 */
void    SvcNetworkMotionDetectNotify(void);

/* =========================================================
 *  设备 ID
 * ========================================================= */
void    SvcNetworkLocalDeviceSet(uint8_t dev_id);
uint8_t SvcNetworkLocalDeviceGet(void);

/* =========================================================
 *  初始化
 * ========================================================= */
int     SvcNetworkInit(void);
int     SvcNetworkDeinit(void);

#endif /* _SVC_NETWORK_H_ */
