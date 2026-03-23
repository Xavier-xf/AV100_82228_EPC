/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2024-04-07 09:37:55
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2024-04-10 10:40:01
 * @FilePath: /net_camera/net_camera.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
//
// Created by michael on 2022/1/21.
//

#ifndef NET_CAMERA_H
#define NET_CAMERA_H
#include "stdbool.h"

#define UPGRADE_TMP_PATH "/tmp/"

#define FILE_SIZE_FLAG "file_size:"
#define UPGRADE_CANCEL_FLAG "Cancel:Upgrade"
#define UPGRADE_ERROR "Upgrade Error"
#define UPGRADE_FINISH "Upgrade Finish"
#define VERIFY_VERSION_PASS "Version Verification Passed"
#define VERIFY_VERSION_CONSISTENT "Version Verification Consistent"
#define START_UPGRADE "Start Upgrade"
#ifndef IPC_MODEL
#define IPC_MODEL "SAT_CAMERA"
#endif
#ifndef LOCAL_VERSION_PATH
#define LOCAL_VERSION_PATH "/app/"
#endif

bool server_update_file_exist(void);

bool create_upgrade_server_task(void);
bool destroy_upgrade_server_task(void);
int connect_server_upgrade(char *upgrade_server_ip);

bool get_outdoor_updata_status(void);
bool create_upgrade_client_task(char *server_ip);
bool upgrade_client_status(void);
bool destroy_upgrade_client_task(void);

bool StartUpgradeApp(char *upgrade_file);
void StartExeUpgrade(void);
#endif // KCV_T701BT_TCP_UPGRADE_H
