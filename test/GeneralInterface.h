/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2023-12-12 09:05:38
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2023-12-16 14:41:40
 * @FilePath: /project_3/src/GeneralInterface.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef _GENERAL_INTERFACE_H_
#define _GENERAL_INTERFACE_H_
#include "time.h"

#define NetworkSlaveId(x) ((x & 0x00) | ((x)-1))
#define network_get_id_indoor_id1(x) ((x & 0x00) | (0x0))

/**
 * @description: 获取时间差
 * @param {timespec} *last_time 基准时间
 * @return {*}  时间差值
 */
unsigned long long DiffClockTimeMs(struct timespec *last_time);

/**
 * @description: 获取时间
 * @param {timespec} *time 时间缓存
 * @return {*}  时间差值
 */
void GetClockTimeMs(struct timespec *time);

/**
 * @description: 音频传输网络协议
 * @param {int} DevId   本地设备ID
 * @param {int} SlaveId 从机设备ID
 * @return {*}
 */
int AudioNetSocketProtocolGet(int DevId, int SlaveId);

/**
 * @description: 视频传输网络协议
 * @param {int} SlaveId 从机设备ID
 * @return {*}
 */
int VideoNetSocketProtocolGet(int SlaveId);
#endif