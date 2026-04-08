/**
 * @file    app_card.h
 * @brief   IC 卡管理（数据库 + 刷卡业务逻辑）
 *
 * 数据持久化：/etc/config/UserCard.cfg
 */
#ifndef _APP_CARD_H_
#define _APP_CARD_H_

#include <stdint.h>

/* =========================================================
 *  常量
 * ========================================================= */
#define CARD_CONFIG_PATH   "/etc/config/UserCard.cfg"
#define CARD_DATA_LEN      5    /* 4 字节 UID + 1 字节异或校验 */
#define CARD_CODE_LEN      4    /* 4 位 PIN 码 */
#define DECK_SIZE_MAX      200  /* 最多存储卡片数量 */
#define CARD_INITIAL_CODE  "0000"

/* 卡片权限位（可组合）*/
#define CARD_PERM_LOCK     0x01  /* 控制门锁 */
#define CARD_PERM_GATE     0x02  /* 控制门闸 */


/* =========================================================
 *  数据结构
 * ========================================================= */
typedef struct {
    char Perm;                /* 权限位：0=空位，CARD_PERM_LOCK/GATE */
    char Data[CARD_DATA_LEN]; /* 卡片 UID（4字节）+ 校验（1字节）    */
    char Code[CARD_CODE_LEN]; /* 4 位 PIN 码                         */
} AppCard;

typedef struct {
    char    DeckSize;
    AppCard Deck[DECK_SIZE_MAX];
} AppCardInfo;

/* =========================================================
 *  卡组管理接口
 * ========================================================= */

/** @brief 卡组初始化（从文件读取）*/
int AppCardDeckInit(void);

/** @brief 格式化卡组（清空所有卡片并保存）*/
int AppCardDeckFormat(void);

/** @brief 保存卡组到文件 */
int AppCardSave(void);

/** @brief 获取卡组信息指针 */
AppCardInfo *AppCardInfoGet(void);

/** @brief 添加卡片（index=-1 自动查找空位）
 *  @return 0=失败 1=成功
 */
int AppCardAdd(int index, char *data, char permissions);

/** @brief 设置卡片权限（permissions=0 等同删除）
 *  @return 0=失败 1=成功
 */
int AppCardSetPerm(int index, char permissions);

/** @brief 搜索卡片
 *  @return -1=未找到，>=0=卡片索引
 */
int AppCardSearch(char *data);

/** @brief 卡片 PIN 码校验
 *  @return 0=失败，非0=权限值
 */
int AppCardCodeVerify(int index, char *code, int code_len);

/** @brief 搜索 PIN 码权限（不能为初始码 "0000"）
 *  @return 0=失败，非0=权限值
 */
int AppCardCodePermission(char *code, int code_len);

/** @brief 获取指定索引的卡片权限 */
char AppCardIndexPerm(int index);

/** @brief 获取卡组权限+卡号数据包（供网络管理发送）
 *  @param deck  输出缓冲区指针（静态，无需释放）
 *  @return 数据大小（字节）
 */
int AppCardDeckPermGet(unsigned char **deck);

/* =========================================================
 *  安防错误计数（供 app_keypad.c 密码开锁调用）
 * ========================================================= */

/** @brief 重置安防错误计数 */
void AppCardSecurityErrorReset(void);

/** @brief 递增安防错误计数（达到上限触发保护）*/
void AppCardSecurityErrorUpdate(void);

/* =========================================================
 *  卡+码组合开锁（CardAndCodeWay）
 *   app_card.c 刷卡后存储卡索引，app_keypad.c 输入密码时读取
 * ========================================================= */

/** @brief 获取当前等待密码验证的卡片索引（-1=无效）*/
int AppCardCodeCardIdxGet(void);

/* =========================================================
 *  开锁异步接口（供 app_keypad.c 调用，与 app_card.c 共用线程）
 * ========================================================= */

/** @brief 异步开锁（与 AppCardHandle 内部 unlock_async 一致）
 *  @param type        GPIO_LOCK_DOOR / GPIO_LOCK_GATE
 *  @param duration_ms 开锁持续时长（毫秒）
 *  @param play_voice  1=播放开锁提示音，0=不播
 */
void AppCardUnlockAsync(int type, int duration_ms, int play_voice);

/* =========================================================
 *  刷卡业务接口（由 drv_card.c 回调）
 * ========================================================= */

/** @brief 处理一次刷卡事件（RC522 读到的原始 4 字节 UID）
 *
 *   - 添加模式（TMR_ADD_CARD 激活）：录入卡片
 *   - 删除模式（TMR_DEL_CARD 激活）：删除卡片
 *   - 普通模式：查询卡片权限，驱动开锁
 */
void AppCardHandle(char *raw_uid4);

/* =========================================================
 *  初始化
 * ========================================================= */

/** @brief 初始化卡片模块（加载数据 + 注册 HAL 回调）*/
int AppCardInit(void);

#endif /* _APP_CARD_H_ */
