/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2023-12-20 08:31:13
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2024-01-23 11:52:05
 * @FilePath: /project_3/common/GpioControl/GpioControl.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef _GPIO_CONTROL_H_
#define _GPIO_CONTROL_H_
/*******************************************************************
 * @Descripttion   :
 * @version        : 1.0.0
 * @Author         : wxj
 * @Date           : 2023-03-14 11:40
 * @LastEditTime   : 2023-03-17 11:56
 *******************************************************************/

#include <stdbool.h>

typedef enum
{
    GPIO_DIR_IN = 0,
    GPIO_DIR_OUT = 1
} GPIO_DIR;

typedef enum
{
    NONE_EDGE,
    RISING_EDGE,
    FALLING_EDGE,
    BOTH_EDGE,
} GPIO_EDGE;

typedef enum
{
    GPIO_LEVEL_LOW = 0,
    GPIO_LEVEL_HIGH = 1,
    GPIO_LEVEL_UNKNOWN,
} GPIO_LEVEL;

/*******************************************************************
 * @brief  : 打开gpio初始化
 * @return  {*}
 * @param {int} pin：引脚
 * @param {GPIO_DIR} dir：方向
 * @param {bool} pull_enable：使能上下拉
 *******************************************************************/
bool GpioOpen(const int pin, GPIO_DIR dir, bool pull_enable);

/*******************************************************************
 * @brief  : 关闭gpio
 * @return  {*}
 * @param {int} pin：引脚
 *******************************************************************/
bool GpioClose(const int pin);

/*******************************************************************
 * @brief  : 设置gpio引脚电平，需要先调用GpioOpen打开初始化gpio
 * @return  {*}
 * @param {int} pin：引脚
 * @param {GPIO_LEVEL} level：设置电平
 *******************************************************************/
bool GpioLevelSet(const int pin, GPIO_LEVEL level);

/*******************************************************************
 * @brief  : 读取gpio引脚电平，需要先调用GpioOpen打开初始化gpio
 * @return  {*}
 * @param {int} pin：引脚
 * @param {GPIO_LEVEL} *level：保存读取到电平的指针
 *******************************************************************/
bool GpioLevelGet(const int pin, GPIO_LEVEL *level);

/*******************************************************************
 * @description: 设置引脚边缘触发
 * @param {int} pin 引脚
 * @param {int} edge    触发条件
 * @return {*}
 *******************************************************************/
int GpioEdge(int pin, GPIO_EDGE edge);

#endif