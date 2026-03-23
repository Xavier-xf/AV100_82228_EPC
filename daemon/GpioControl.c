/*******************************************************************
 * @Descripttion   :
 * @version        : 1.0.0
 * @Author         : wxj
 * @Date           : 2023-03-17 10:49
 * @LastEditTime   : 2023-03-17 13:45
 *******************************************************************/
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include "GpioControl.h"

#define FILE_PATH_MAX 64

/*******************************************************************
 * @brief  : 打开gpio初始化
 * @return  {*}
 * @param {int} pin：引脚
 * @param {GPIO_DIR} dir：方向
 * @param {bool} pull_enable：使能上下拉
 *******************************************************************/
bool GpioOpen(const int pin, GPIO_DIR dir, bool pull_enable)
{
    int Fd = -1;
    char Value[FILE_PATH_MAX] = {0};
    char Path[FILE_PATH_MAX] = {0};

    memset(Path, 0, FILE_PATH_MAX);
    sprintf(Path, "/sys/class/gpio/gpio%d", pin);

    if (access(Path, F_OK))
    {
        memset(Path, 0, FILE_PATH_MAX);
        sprintf(Path, "/sys/class/gpio/export");

        Fd = open(Path, O_WRONLY);
        if (Fd < 0)
        {
            printf("Open gpio%d export fail\n", pin);
            return false;
        }

        memset(Value, 0, FILE_PATH_MAX);
        sprintf(Value, "%d\n", pin);

        if (write(Fd, Value, strlen(Value)) < 0)
        {
            close(Fd);
            printf("create gpio%d fail\n", pin);
            return false;
        }
        close(Fd);
    }

    memset(Path, 0, FILE_PATH_MAX);
    sprintf(Path, "/sys/class/gpio/gpio%d/direction", pin);
    Fd = open(Path, O_WRONLY);
    if (Fd < 0)
    {
        printf("gpio%d direction setting failed \n", pin);
        return false;
    }

    memset(Value, 0, sizeof(Value));
    int len = sprintf(Value, "%s", dir == GPIO_DIR_IN ? "in" : "out");

    if (write(Fd, Value, len) < 0)
    {
        printf("diection write gpio%d %s faild \n", pin, Value);
        close(Fd);
        return false;
    }
    close(Fd);

    memset(Path, 0, FILE_PATH_MAX);
    sprintf(Path, "/sys/class/gpio/gpio%d/pull_enable", pin);

    Fd = open(Path, O_WRONLY);
    if (Fd < 0)
    {
        printf("gpio%d pull_enable setting failed \n", pin);
        return false;
    }

    if (write(Fd, pull_enable ? "1" : "0", 1) < 0)
    {
        printf("gpio pull write gpio%d %d faild \n", pin, pull_enable);
        close(Fd);
        return false;
    }

    close(Fd);
    return true;
}

/*******************************************************************
 * @brief  : 关闭gpio
 * @return  {*}
 * @param {int} pin：引脚
 *******************************************************************/
bool GpioClose(const int pin)
{
    int Fd = -1;
    char Value[FILE_PATH_MAX] = {0};
    char Path[FILE_PATH_MAX] = {0};

    memset(Path, 0, FILE_PATH_MAX);
    sprintf(Path, "/sys/class/gpio/gpio%d", pin);

    if (!access(Path, F_OK))
    {
        memset(Path, 0, FILE_PATH_MAX);
        sprintf(Path, "/sys/class/gpio/unexport");
        Fd = open(Path, O_WRONLY);
        if (Fd < 0)
        {
            printf("Open gpio%d unexport fail\n", pin);
            return false;
        }
        memset(Value, 0, FILE_PATH_MAX);
        sprintf(Value, "%d\n", pin);
        if (write(Fd, Value, strlen(Value)) < 0)
        {
            close(Fd);
            printf("remove gpio%d fail\n", pin);
            return false;
        }
        close(Fd);
    }
    return true;
}

/*******************************************************************
 * @brief  : 设置gpio引脚电平，需要先调用GpioOpen打开初始化gpio
 * @return  {*}
 * @param {int} pin：引脚
 * @param {GPIO_LEVEL} level：设置电平
 *******************************************************************/
bool GpioLevelSet(const int pin, GPIO_LEVEL level)
{
    char Path[FILE_PATH_MAX] = {0};

    memset(Path, 0, FILE_PATH_MAX);
    sprintf(Path, "/sys/class/gpio/gpio%d/value", pin);
    int Fd = open(Path, O_WRONLY);

    if (Fd < 0)
    {
        printf("gpio%d set Value w open failed \n", pin);
        return false;
    }

    if (write(Fd, (level == GPIO_LEVEL_LOW) ? "0" : "1", 1) < 0)
    {
        printf("gpio%d write %d failed \n", pin, level == GPIO_LEVEL_LOW ? 0 : 1);
        close(Fd);
        return false;
    }

    close(Fd);
    return true;
}

/*******************************************************************
 * @brief  : 读取gpio引脚电平，需要先调用GpioOpen打开初始化gpio
 * @return  {*}
 * @param {int} pin：引脚
 * @param {GPIO_LEVEL} *level：保存读取到电平的指针
 *******************************************************************/
bool GpioLevelGet(const int pin, GPIO_LEVEL *level)
{
    char Value[FILE_PATH_MAX] = {0};
    char Path[FILE_PATH_MAX] = {0};

    memset(Path, 0, FILE_PATH_MAX);
    sprintf(Path, "/sys/class/gpio/gpio%d/value", pin);

    int Fd = open(Path, O_RDONLY);
    if (Fd < 0)
    {
        printf("gpio%d set Value r open failed \n", pin);
        return false;
    }

    memset(Value, 0, sizeof(Value));
    if (read(Fd, Value, 1) < 0)
    {
        printf("gpio%d read failed \n", pin);
        close(Fd);
        return false;
    }

    close(Fd);

    *level = (Value[0] == '0') ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
    return true;
}

/**
 * @description: 设置引脚边缘触发
 * @param {int} pin 引脚
 * @param {int} edge    触发条件
 * none：引脚为输入，不是中断引脚
 * rising：引脚为中断输入，上升沿触发
 * falling：引脚为中断输入，下降沿触发
 * both：引脚为中断输入，边沿触发
 * @return {*}
 */
int GpioEdge(int pin, GPIO_EDGE edge)
{
    const char DirStr[][9] = {"none\0", "rising\0", "falling\0", "both"};
    char Path[64];
    int Fd;

    snprintf(Path, sizeof(Path), "/sys/class/gpio/gpio%d/edge", pin);
    Fd = open(Path, O_WRONLY);
    if (Fd < 0)
    {
        printf("Failed to open gpio edge for writing!\n");
        return -1;
    }

    if (write(Fd, &DirStr[edge], strlen(DirStr[edge])) < 0)
    {
        printf("Failed to set edge!\n");
        return -1;
    }

    close(Fd);
    return 0;
}
