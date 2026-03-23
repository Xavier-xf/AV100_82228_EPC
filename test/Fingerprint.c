/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2024-01-04 15:08:22
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2024-01-17 09:17:41
 * @FilePath: /project_3/common/Fingerprint/Fingerprint.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "GeneralInterface.h"
#include "Fingerprint.h"
#include "UartControl.h"
// #include "GpioControl.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>

static int UartFd = -1;
#define RETURN_TIMEOUT 5000

static bool FingerReadData(FingerDataAck *Buffer)
{
    int ReadLen = 0;
    int FingerHeadLen = sizeof(FingerHead);
    memset(Buffer, 0, sizeof(FingerDataAck));
    if (UartBufferSize(UartFd) >= FingerHeadLen && (ReadLen = UartRead(UartFd, (char *)&(Buffer->Format), FingerHeadLen)) == FingerHeadLen) // 读取包头至包长度的数据
    {
        if (PACKAGE_HEADER == (uint16_t)((Buffer->Format.Header[0] << 8) | Buffer->Format.Header[1]))
        {
            if ((Buffer->Format.Addr[0] & Buffer->Format.Addr[1] & Buffer->Format.Addr[2] & Buffer->Format.Addr[3]) != 0xFF)
                return false;

            if (Buffer->Format.Type != DATA_PACKAGE && Buffer->Format.Type != ACK_PACKAGE && Buffer->Format.Type != END_PACKAGE)
                return false;

            uint16_t PackLen = (((uint16_t)(Buffer->Format.Len[0]) << 8) | (uint16_t)(Buffer->Format.Len[1] & 0xFF));

            if ((UartRead(UartFd, Buffer->Format.Type != ACK_PACKAGE ? (char *)Buffer->Data : (char *)&(Buffer->Affirm), PackLen)) != PackLen)
                return false;

            uint32_t CheckSum = Buffer->Format.Type + PackLen + Buffer->Affirm;

            uint16_t DataLen = Buffer->Format.Type == ACK_PACKAGE ? PackLen - AFFIRM_CODE_LEN : PackLen;

            for (int i = 0; i < DataLen - CHECK_CODE_LEN; i++)
            {
                CheckSum += Buffer->Data[i];
            }

            if (CheckSum == (((uint16_t)(Buffer->Data[DataLen - 2]) << 8) | (uint16_t)(Buffer->Data[DataLen - 1] & 0xFF)))
            {
                Buffer->CheckSum[0] = Buffer->Data[DataLen - 2];
                Buffer->CheckSum[1] = Buffer->Data[DataLen - 1];

                printf("RECEIVE DATA:");
                printf("[ %2x ]", Buffer->Format.Type);
                printf("[ %2x ][ %2x ]", Buffer->Format.Len[0], Buffer->Format.Len[1]);
                printf("[ %2x ]", Buffer->Affirm);
                for (int i = 0; i < DataLen - CHECK_CODE_LEN; i++)
                {
                    printf("[ %2x ]", Buffer->Data[i]);
                }
                printf("[ %2x ][ %2x ]", Buffer->Data[DataLen - 2], Buffer->Data[DataLen - 1]);
                printf("\n");
                return true;
            }
            else
            {
                printf("RECEIVE DATA:");
                printf("[ %2x ]", Buffer->Format.Type);
                printf("[ %2x ][ %2x ]", Buffer->Format.Len[0], Buffer->Format.Len[1]);
                printf("[ %2x ]", Buffer->Affirm);
                for (int i = 0; i < DataLen - CHECK_CODE_LEN; i++)
                {
                    printf("[ %2x ]", Buffer->Data[i]);
                }
                printf("[ %2x ][ %2x ]", Buffer->Data[DataLen - 2], Buffer->Data[DataLen - 1]);
                printf("\n");
            }
        }
    }
    else if (ReadLen > 0)
    {
        printf("RECEIVE DATA ERROR LEN: %d ", ReadLen);
        printf("[ %2x ][ %2x ]", Buffer->Format.Header[0], Buffer->Format.Header[1]);
        printf("[ %2x ][ %2x ][ %2x ][ %2x ]\n", Buffer->Format.Addr[0], Buffer->Format.Addr[1], Buffer->Format.Addr[2], Buffer->Format.Addr[3]);
        printf("\n");
    }
    return false;
}

static void FingerWriteCmd(uint8_t Type, uint8_t Cmd, uint8_t *Data, uint16_t Size)
{
    uint16_t DataLen = FINGER_BASE_DATA_LEN + Size;
    uint8_t Buffer[DataLen];
    uint32_t CheckSum = 0;
    Buffer[0] = (PACKAGE_HEADER >> 8) & 0xFF;
    Buffer[1] = PACKAGE_HEADER & 0xFF;
    Buffer[2] = (DEVICE_ADDR >> 24) & 0xFF;
    Buffer[3] = (DEVICE_ADDR >> 16) & 0xFF;
    Buffer[4] = (DEVICE_ADDR >> 8) & 0xFF;
    Buffer[5] = DEVICE_ADDR & 0xFF;
    Buffer[6] = Type;
    Buffer[7] = ((Size + 3) >> 8) & 0xFF;
    Buffer[8] = (Size + 3) & 0xFF;
    Buffer[9] = Cmd;
    CheckSum = Type + (Size + 3) + Cmd;
    for (int i = 0; i < Size; i++)
    {
        Buffer[10 + i] = Data[i];
        CheckSum += Data[i];
    }
    Buffer[DataLen - 2] = (CheckSum >> 8) & 0xFF;
    Buffer[DataLen - 1] = CheckSum & 0xFF;
    UartWrite(UartFd, (char *)Buffer, DataLen);

    printf("\n\rsend Data :");
    for (int i = 0; i < DataLen; i++)
    {
        printf("[ %x ]", Buffer[i]);
    }
    printf("\n");
}

static uint16_t ValidTemplateNum(void)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);
    FingerWriteCmd(CMD_PACKAGE, CODE_VALID_TEMPLATE_NUM, NULL, 0);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {
            uint16_t Nmber = (((uint16_t)Buffer.Data[0] << 8) | ((uint16_t)Buffer.Data[1]));
            printf("有效模板数量获取%s\n", Buffer.Affirm == 0x00 ? "成功" : "失败");
            return Buffer.Affirm == 0x00 ? Nmber : 0x00;
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("有效模板数量获取失败\n");
    return 0x00;
}

static uint16_t AutoVerifyFinger(uint16_t Id)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);

    uint8_t Data[5];
    Data[0] = 0x02;
    Data[1] = (Id >> 8) & 0xFF;
    Data[2] = Id & 0xFF;
    Data[3] = 0x00;
    Data[4] = 0b00000010;
    FingerWriteCmd(CMD_PACKAGE, CODE_AUTO_IDENTIFY, Data, 5);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {
            if (Buffer.Affirm == 0x00 && Buffer.Data[0] == 0x05)
            {
                printf("比对成功\n");
                return (((uint16_t)Buffer.Data[1] << 8) | ((uint16_t)Buffer.Data[2]));
            }
            else if (Buffer.Affirm == 0x00 && Buffer.Data[0] == 0x00)
                printf("指令合法性检测成功\n");
            else if (Buffer.Affirm == 0x00 && Buffer.Data[0] == 0x01)
                printf("录入指纹获取图像成功\n");
            else
                break;
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("比对失敗\n");
    return 0xFFFF;
}

static bool AutoEnrool(uint16_t Id)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);

    uint8_t Data[5];
    Data[0] = (Id >> 8) & 0xFF;
    Data[1] = Id & 0xFF;
    Data[2] = 0x02;
    Data[3] = 0x00;
    Data[4] = 0b10011100;
    FingerWriteCmd(CMD_PACKAGE, CODE_AUTO_ENROLL, Data, 5);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {
            printf("receive Data:");
            printf("[ %x ]", Buffer.Format.Type);
            printf("[ %x ][ %x ]", Buffer.Format.Len[0], Buffer.Format.Len[1]);
            uint16_t DataLen = (((uint16_t)(Buffer.Format.Len[0]) << 8) | (uint16_t)(Buffer.Format.Len[1] & 0xFF)) - AFFIRM_CODE_LEN;
            for (int i = 0; i < DataLen - CHECK_CODE_LEN; i++)
            {
                printf("[ %x ]", Buffer.Data[i]);
            }
            printf("[ %x ][ %x ]", Buffer.CheckSum[0], Buffer.CheckSum[1]);
            printf("\n");
            if (Buffer.Data[0] == 0x00 && Buffer.Data[1] == 0x00)
            {
                if (Buffer.Affirm == 0x00)
                    printf("指令合法性检测成功,并进入第一次指纹录入\n");
                else
                    break;
            }
            else if (Buffer.Affirm == 0x00 && Buffer.Data[0] == 0x01)
                printf("等待第[%d]次彩图成功\n", Buffer.Data[1]);
            else if (Buffer.Data[0] == 0x02)
            {
                if (Buffer.Affirm == 0x00)
                    printf("等待第[%d]次生成特征成功\n", Buffer.Data[1]);
                else
                    break;
            }
            else if (Buffer.Affirm == 0x00 && Buffer.Data[0] == 0x03)
                printf("第[%d]次等待手指离开\n", Buffer.Data[1]);
            else if (Buffer.Data[0] == 0x04 && Buffer.Data[1] == 0xF0)
            {
                if (Buffer.Affirm == 0x00)
                    printf("合成模板成功\n");
                else
                {
                    printf("合成模板失败\n");
                    break;
                }
            }
            else if (Buffer.Data[0] == 0x05 && Buffer.Data[1] == 0xF1)
            {
                if (Buffer.Affirm == 0x00)
                    printf("没有相同指纹\n");
                else if (Buffer.Affirm == 0x27)
                {
                    printf("有相同指纹\n");
                    break;
                }
            }
            else if (Buffer.Data[0] == 0x06 && Buffer.Data[1] == 0xF2)
            {
                if (Buffer.Affirm == 0x00)
                {
                    printf("模板数据存储成功：%d\n", Id);
                    return true;
                }
                else
                {
                    printf("模板数据存储失败\n");
                    break;
                }
            }
            else if (Buffer.Affirm == 0x26)
            {
                printf("超时\n");
                break;
            }
            else if (Buffer.Affirm == 0x22)
            {
                printf("指纹模板非空\n");
                break;
            }
            else
            {
                break;
            }
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("指纹模板录入失败\n");
    return false;
}

static bool DeleteTemplate(uint16_t Page, uint16_t Num)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);

    uint8_t Data[4];
    Data[0] = (Page >> 8) & 0xFF;
    Data[1] = Page & 0xFF;
    Data[2] = (Num >> 8) & 0xFF;
    Data[3] = Num & 0xFF;
    FingerWriteCmd(CMD_PACKAGE, CODE_DELETE_CHAR, Data, 4);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {
            printf("刪除指紋%s\n", Buffer.Affirm == 0x00 ? "成功" : "失败");
            return (Buffer.Affirm == 0x00);
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("刪除指紋失败\n");
    return false;
}

static bool EmptyTemplate(void)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);
    FingerWriteCmd(CMD_PACKAGE, CODE_EMPTY, NULL, 0);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {
            printf("清空指紋%s\n", Buffer.Affirm == 0x00 ? "成功" : "失败");
            return (Buffer.Affirm == 0x00);
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("清空指紋失败\n");
    return false;
}

static bool ReadIndexTable(uint8_t Page, FingerDataAck *DataAck)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);
    FingerWriteCmd(CMD_PACKAGE, CODE_READ_INDEX_TABLE, &Page, 1);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {

            printf("索引列表读取%s\n", Buffer.Affirm == 0x00 ? "成功" : "失败");
            *DataAck = Buffer.Affirm == 0x00 ? Buffer : *DataAck;
            return (Buffer.Affirm == 0x00);
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("索引列表读取失败\n");
    return false;
}

static bool LightSetting(LightCode Code, uint8_t Speed, LightColor Color, uint8_t Times)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);

    uint8_t Data[4];
    Data[0] = Code;
    Data[1] = Speed;
    Data[2] = Color;
    Data[3] = Times;
    FingerWriteCmd(CMD_PACKAGE, CODE_LIGHT_SETTING, Data, 4);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {
            printf("灯光设置%s\n", Buffer.Affirm == 0x00 ? "成功" : "失败");
            return (Buffer.Affirm == 0x00);
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("灯光设置失败\n");
    return false;
}

static bool Cancel(void)
{
    FingerDataAck Buffer;
    struct timespec time;
    GetClockTimeMs(&time);
    FingerWriteCmd(CMD_PACKAGE, CODE_CANCEL, NULL, 0);
    while (1)
    {
        if (FingerReadData(&Buffer))
        {
            printf("指令中断退出%s\n", Buffer.Affirm == 0x00 ? "成功" : "失败");
            return (Buffer.Affirm == 0x00);
        }
        else if (DiffClockTimeMs(&time) > RETURN_TIMEOUT)
        {
            break;
        }
        usleep(1000);
    }
    printf("指令中断退出失败\n");
    return false;
}

static void *DrvFingerprintThread(void *arg)
{
    printf("EmptyTemplate:%s\n", EmptyTemplate() ? "succee" : "fail");
    while (1)
    {
        /* code */
    }
    return NULL;
}

/**
 * @description: 指纹驱动初始化
 * @return {*}
 */
int FingerprintInit(void)
{
    UartFd = UartOpen("ttySAK1", 57600, 8, 1, 'n');
    if (UartFd < 0)
    {
        printf("open ttySAK1 faild \n");
        usleep(1000 * 1000);
        return false;
    }
    // GpioOpen(55, GPIO_DIR_IN, 1);
    pthread_t Thread;
    pthread_create(&Thread, NULL, DrvFingerprintThread, NULL);
    pthread_detach(Thread);
    return 0;
}