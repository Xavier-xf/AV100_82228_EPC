/**
 * @file    drv_card.h
 * @brief   RC522 RFID 读卡器 HAL 驱动
 *
 * 职责：
 *   - 加载内核模块（sim_idrv.ko + RC522.ko）
 *   - 轮询 /dev/RC522，读取 4 字节卡片 UID
 *   - 通过回调将原始数据上报，不含任何业务逻辑
 *
 * 移植自旧版 DrvSwipeCard.c + RC522Card.c（驱动部分）。
 */
#ifndef _DRV_CARD_H_
#define _DRV_CARD_H_

/**
 * @brief 刷卡原始数据回调（4 字节 UID）
 * @param raw_uid4  4 字节卡片 UID（不含校验位，由业务层补充）
 */
typedef void (*DrvCardCallback)(char *raw_uid4);

/**
 * @brief 注册刷卡回调（在 DrvCardInit 之前调用）
 */
void DrvCardSetCallback(DrvCardCallback cb);

/**
 * @brief 初始化读卡器：加载内核模块，开启轮询线程
 * @return 0=成功，-1=初始化失败（无内核模块或设备不可用）
 */
int DrvCardInit(void);

#endif /* _DRV_CARD_H_ */
