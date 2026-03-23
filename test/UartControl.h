/*
 * @Description:
 * @Version: 1.0
 * @Autor: wxj
 * @Date: 2022-07-22 11:11:01
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2023-12-26 09:52:16
 */
#ifndef _UART_CTRL_H_
#define _UART_CTRL_H_
#include <stdbool.h>
#include <stdio.h>
/***
**   日期:2022-05-30 17:41:52
**   作者: leo.liu
**   函数作用：打开串口
**   参数说明:
***/
int UartOpen(char *dev, int speed, int data_bits, int stop_bits, int parity);
/***
**   日期:2022-05-30 17:42:38
**   作者: leo.liu
**   函数作用：发送串口数据
**   参数说明:
***/
int UartWrite(int fd, char *data, int size);
/***
**   日期:2022-05-30 17:46:47
**   作者: leo.liu
**   函数作用：读取串口数据
**   参数说明:
***/
int UartRead(int fd, char *data, int size);
/***
**   日期:2022-05-30 17:46:55
**   作者: leo.liu
**   函数作用：关闭串口
**   参数说明:
***/
bool UartClose(int fd);
/***
**   日期:2022-09-21 17:46:55
**   作者: leo.wu
**   函数作用：清空串口清空输入输出缓存
**   参数说明:
***/
bool UartClear(int fd);

/***
**   日期:2023-01-09 09:42:55
**   作者: leo.wu
**   函数作用：读取串口缓存
**   参数说明:
***/
int UartBufferSize(int fd);
#endif