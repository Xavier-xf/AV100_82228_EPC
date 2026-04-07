/**
 * @file    svc_net_manage.h
 * @brief   网络卡片管理服务（TCP 4321 端口）
 *
 * 移植自旧版 UserNetManage.c + NetManageCom.c。
 * 提供室内机 APP 对 IC 卡数据库的远程管理能力：
 *   - 进入/退出添加卡模式
 *   - 删除卡片
 *   - 获取卡组列表
 *   - 设置卡片权限
 *
 * 协议格式（与旧版相同，TCP 4321）：
 *   短包（8 字节）：[0xAA][dev][src][cmd][arg1][arg2][csum][0xCC]
 *   长包（1288字节）：[0xBB][dev][src][cmd][arg][...data...][0xCC]
 */
#ifndef _SVC_NET_MANAGE_H_
#define _SVC_NET_MANAGE_H_

/* ---- 命令字（对应旧版 NetManageEvent）---- */
#define NET_MGR_ADD_CARD      0x10  /* 进入添加卡模式     */
#define NET_MGR_DEL_CARD      0x11  /* 删除卡片           */
#define NET_MGR_VERIFY_CARD   0x12  /* 校验卡片（预留）   */
#define NET_MGR_GET_CARD      0x13  /* 获取卡组数据       */
#define NET_MGR_SET_CARD_PERM 0x14  /* 设置卡片权限       */
#define NET_MGR_EXIT_CARD     0x15  /* 退出添加卡模式     */
#define NET_MGR_ACCESS_DENIED 0x90  /* 拒绝访问           */

/**
 * @brief 发送短包应答
 * @param cmd   命令字
 * @param arg1  参数1
 * @param arg2  参数2
 */
void SvcNetManageSendShort(unsigned char cmd,
                           unsigned char arg1,
                           unsigned char arg2);

/**
 * @brief 查询是否处于网管连接状态
 * @return 1=已连接，0=未连接
 */
int SvcNetManageConnected(void);

/**
 * @brief 初始化网管服务（启动 TCP 4321 监听线程）
 * @return 0=成功，-1=失败
 */
int SvcNetManageInit(void);

#endif /* _SVC_NET_MANAGE_H_ */
