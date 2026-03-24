/**
 * @file    app_upgrade.h
 * @brief   OTA 固件升级业务（完整流程）
 *
 * ===================== 升级协议说明 =====================
 *
 *  室内机通过以太网 Raw Socket（协议号 0xFFFF）发送升级帧。
 *
 *  【长包（数据包）DataLen > 8】
 *    DP[0..3] = 包序号 index（从 1 开始）
 *    DP[4..7] = 本包数据长度 len
 *    DP[8..]  = 实际升级数据
 *
 *  【短包（控制包）DataLen <= 8】
 *    Arg[0] & 0x01 → 查询在线状态     → 应答 Arg1=1, Arg2=1
 *    Arg[0] & 0x02 → 升级执行命令
 *      Arg[1] & 0x01 → 数据接收完毕，开始执行升级
 *      Arg[1] & 0x02 → 升级失败，清理并取消
 *
 *  【应答帧格式】
 *    Cmd = UpgraedOutdoorEvent(0x62)
 *    Arg1 = 1: 在线确认
 *    Arg1 = 2: 开始执行升级（应答后才执行）
 *    Arg1 = 3: 升级失败
 *    Arg2 = 1: 固定值
 *
 * ===================== 升级文件说明 =====================
 *
 *  网络升级：/tmp/av100.update（.update 格式的 tar.gz 包）
 *  SD 卡升级：/mnt/sd/cbin.update（文件名必须为 cbin.update）
 *
 *  包内结构：
 *    image/           目录
 *    image/image.tar.gz   实际固件压缩包
 *    image/image.sha1     SHA1 校验文件
 *
 * ===================== 状态机 =====================
 *
 *  IDLE → RECEIVING（收到第1个数据包）
 *       → VERIFYING（短包 UpdateOver+UpdataFinish）
 *       → FLASHING（verify_sha1 通过后）
 *       → IDLE（成功或失败后重置）
 */
#ifndef _APP_UPGRADE_H_
#define _APP_UPGRADE_H_

#include <stdint.h>

/* 升级包文件路径 */
#define UPGRADE_PACK_NET   "/tmp/av100.update"   /* 网络升级包 */
#define UPGRADE_PACK_SD    "/mnt/sd/cbin.update" /* SD卡升级包 */

/* 升级应答 Arg1 值（与室内机协议对应）*/
#define UPGRADE_REPLY_ONLINE   1   /* 在线状态应答 */
#define UPGRADE_REPLY_EXECUTE  2   /* 确认执行升级 */
#define UPGRADE_REPLY_FAIL     3   /* 升级失败 */

/* =========================================================
 *  升级状态枚举（供外部查询）
 * ========================================================= */
typedef enum {
    UPGRADE_STATE_IDLE       = 0,  /* 空闲，未在升级 */
    UPGRADE_STATE_RECEIVING  = 1,  /* 正在接收数据包 */
    UPGRADE_STATE_PROCESSING = 2,  /* 解压/验证/刷写中 */
    UPGRADE_STATE_DONE       = 3,  /* 升级完成，等待重启 */
    UPGRADE_STATE_ERROR      = 4,  /* 升级失败 */
} AppUpgradeState;

/* =========================================================
 *  接口
 * ========================================================= */

/**
 * @brief 处理来自室内机的长包（数据帧）
 *
 *   index = DP[0..3], len = DP[4..7], data = DP[8..]
 *
 * @param sender_dev  发送方设备 ID（用于失败应答）
 * @param index       包序号（从1开始）
 * @param len         数据长度
 * @param data        数据指针
 * @return 1=成功  0=失败（会自动发送失败应答）
 */
int AppUpgradeHandleLongPack(uint8_t sender_dev,
                              uint32_t index, uint32_t len,
                              const uint8_t *data);

/**
 * @brief 处理来自室内机的短包（控制帧）
 *
 *   ctrl_arg0 = Packet.Data.Arg[0]
 *   ctrl_arg1 = Packet.Data.Arg[1]
 *
 * 此函数内部处理：
 *   ① 查询在线状态 → 立即应答
 *   ② 升级执行命令 → 应答后在后台线程执行升级（不阻塞调用方）
 *   ③ 升级失败取消 → 清理临时文件
 */
void AppUpgradeHandleCtrlPack(uint8_t sender_dev,
                               uint8_t ctrl_arg0, uint8_t ctrl_arg1);

/**
 * @brief 触发 SD 卡升级（检测到 /mnt/sd/cbin.update 时调用）
 * @return 1=升级启动成功  0=文件不存在或已在升级中
 */
int AppUpgradeFromSD(void);

/**
 * @brief 查询当前升级状态
 */
AppUpgradeState AppUpgradeGetState(void);

/**
 * @brief 初始化升级模块
 */
int AppUpgradeInit(void);

#endif /* _APP_UPGRADE_H_ */
