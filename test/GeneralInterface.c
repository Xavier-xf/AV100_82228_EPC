/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2023-12-12 09:05:34
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2023-12-16 13:44:48
 * @FilePath: /project_3/src/GeneralInterface.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "GeneralInterface.h"

/**
 * @description: 获取时间差
 * @param {timespec} *last_time 基准时间
 * @return {*}  时间差值
 */
unsigned long long DiffClockTimeMs(struct timespec *last_time)
{
    struct timespec curr_time;
    unsigned long long diff;
    clock_gettime(CLOCK_MONOTONIC, &curr_time);
    diff = (curr_time.tv_sec - last_time->tv_sec) * 1000 + (curr_time.tv_nsec - last_time->tv_nsec) / 1000000;

    return diff;
}

/**
 * @description: 获取时间
 * @param {timespec} *time 时间缓存
 * @return {*}  时间差值
 */
void GetClockTimeMs(struct timespec *time)
{
    clock_gettime(CLOCK_MONOTONIC, time);
    return;
}

/**
 * @description: 音频传输网络协议
 * @param {int} DevId   本地设备ID
 * @param {int} SlaveId 从机设备ID
 * @return {*}
 */
int AudioNetSocketProtocolGet(int DevId, int SlaveId)
{
#define AUDIO_NET_PROTOCOL_BASE 0X2600
    int MstartId = NetworkSlaveId(DevId);
    int Vol = 0;
    if (MstartId < SlaveId)
    {
        Vol = (SlaveId << 4) | MstartId;
    }
    else
    {
        Vol = (MstartId << 4) | SlaveId;
    }
    return AUDIO_NET_PROTOCOL_BASE | Vol;
}

/**
 * @description: 视频传输网络协议
 * @param {int} SlaveId 从机设备ID
 * @return {*}
 */
int VideoNetSocketProtocolGet(int SlaveId)
{
#define VIDEO_NET_PROTOCOL_BASE 0X1600
    return VIDEO_NET_PROTOCOL_BASE | SlaveId;
}
