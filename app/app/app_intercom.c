/**
 * @file    app_intercom.c
 * @brief   对讲业务状态机（互斥锁版，无原子操作）
 */
#include "app_intercom.h"
#include "app_upgrade.h"
#include "event_bus.h"
#include "svc_network.h"
#include "drv_gpio.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>


static void on_upgrade_cmd(EventId id, const void *arg, size_t len)
{
    (void)id; (void)len;
    const NetUpgradeArg *upg = (const NetUpgradeArg *)arg;

    if (upg->is_long_pack) {
        /* 长包：数据帧（原协议 DataLen > 8）
         * arg1 = 包序号（DP[0..3]）, arg2 = 数据长度（DP[4..7]）
         * data = DP[8..] */
        AppUpgradeHandleLongPack(upg->sender_dev,
                                  upg->arg1, upg->arg2, upg->data);
    } else {
        /* 短包：控制帧（原协议 DataLen <= 8）
         * ctrl_arg0 = Arg[0], ctrl_arg1 = Arg[1] */
        AppUpgradeHandleCtrlPack(upg->sender_dev,
                                   upg->ctrl_arg0, upg->ctrl_arg1);
    }
}

int AppIntercomInit(void)
{
    EventBusSubscribe(EVT_NET_UPGRADE_CMD,           on_upgrade_cmd);
    printf("[AppIntercom] init ok\n");
    return 0;
}
