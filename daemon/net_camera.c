/*server.c*/
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <pthread.h>
#include "net_camera.h"
#include "GpioControl.h"

#define FILE_MAX_LEN 256

#define HELLO_WORLD_SERVER_PORT 6666 // 端口号
#define LENGTH_OF_LISTEN_QUEUE 20
#define BUFFER_SIZE 1024

static char *GetLocalVersion(void)
{
    DIR *dp;
    struct dirent *entry;
    struct stat statbuf;
    static char *version = NULL;
    if (version == NULL)
    {
        if ((version = malloc(32 * sizeof(char))))
        {
            memset(version, 0, 32);
            if ((dp = opendir(LOCAL_VERSION_PATH)) == NULL)
            {
                fprintf(stderr, "cannot open directory: %s\n", LOCAL_VERSION_PATH);
                return 0;
            }
            chdir(LOCAL_VERSION_PATH);

            while ((entry = readdir(dp)) != NULL)
            {
                lstat(entry->d_name, &statbuf); // 获取文件属性
                if (!S_ISDIR(statbuf.st_mode))
                {
                    if (strncmp(entry->d_name, IPC_MODEL, strlen(IPC_MODEL)) == 0)
                    {
                        sprintf(version, "%s", entry->d_name);
                        break;
                    }
                }
            }
            chdir("..");
            closedir(dp);
        }
    }
    return version;
}

int VerifyUpgradeVersion(char *data)
{
    if (strlen(data) < strlen(IPC_MODEL))
        return -1;

    if (strncmp(data, IPC_MODEL, strlen(IPC_MODEL)) != 0)
        return -1;

    char *local_ver = GetLocalVersion();
    printf("\nLocal Model:%s len:%d \nUpgrade Model:%s len:%d\n", local_ver, strlen(local_ver), data, strlen(data));
    if (local_ver && (strlen(local_ver) == strlen(data)) && (strncmp(data, local_ver, strlen(local_ver)) == 0))
    {
        // printf("%s,%d\n", __func__, __LINE__);
        return 0;
    }

    return 1;
}

static int listen_socket = 0;
bool CreateUpgradeTcpServer(void)
{
    // set socket's address information
    // 设置一个socket地址结构server_addr,代表服务器internet的地址和端口
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htons(INADDR_ANY);
    server_addr.sin_port = htons(HELLO_WORLD_SERVER_PORT);

    // create a stream socket
    // 创建用于internet的流协议(TCP)socket，用server_socket代表服务器向客户端提供服务的接口
    listen_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0)
    {
        printf("Create Socket Failed!\n");
        return false;
    }

    // 把socket和socket地址结构绑定
    if (bind(listen_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)))
    {
        printf("Server Bind Port: %d Failed!\n", HELLO_WORLD_SERVER_PORT);
        return false;
    }

    // server_socket用于监听
    if (listen(listen_socket, LENGTH_OF_LISTEN_QUEUE))
    {
        printf("Server Listen Failed!\n");
        return false;
    }
    return true;
}
#define STEP_START 0
#define STEP_VERIFY_VERSION 1
#define STEP_START_UPGRADE 2
#define STEP_WAIT_REMOTE_FILE_INTEGRITY_CHECK 3
#define STEP_STOP 4
#define STEP_FINISH 5

static int upgrade_step_flag = STEP_START;

static void ClearTmpFlag(void)
{
    upgrade_step_flag = STEP_START;
}

static int SelectRecvSocket(int socket, char *buffer)
{
    fd_set read_fds;
    struct timeval timeout;
    int retval;

    FD_ZERO(&read_fds);
    FD_SET(socket, &read_fds);

    timeout.tv_sec = 0; // 设置超时时间为5秒
    timeout.tv_usec = 5000;
    retval = select(socket + 1, &read_fds, NULL, NULL, &timeout);
    if (retval <= 0)
    {
        return -1;
    }

    if (!FD_ISSET(socket, &read_fds))
    {
        return -1;
    }

    retval = recv(socket, buffer, BUFFER_SIZE, 0);
    return retval;
}

static void *UpgradeServerTask(void *arg)
{
    ClearTmpFlag();

    if (CreateUpgradeTcpServer() == false)
    {
        return NULL;
    }

    char buffer[BUFFER_SIZE];
    int communication_socket = -1;
    struct sockaddr_in client_addr;
    int length = sizeof(client_addr);
    unsigned long update_file_size = 0;
    char update_file_name[128] = {0};
    // 服务器端一直运行用以持续为客户端提供服务

    static FILE *fp = NULL;
    static int received_length = 0;

    bool *run = (bool *)arg;
    while (*run)
    {
        while (communication_socket == -1)
        {
            // 定义客户端的socket地址结构client_addr，当收到来自客户端的请求后，调用accept
            // 接受此请求，同时将client端的地址和端口等信息写入client_addr中
            // 接受一个从client端到达server端的连接请求,将客户端的信息保存在client_addr中
            // 如果没有连接请求，则一直等待直到有连接请求为止，这是accept函数的特性，可以
            // 用select()来实现超时检测
            // accpet返回一个新的socket,这个socket用来与此次连接到server的client进行通信
            // 这里的new_server_socket代表了这个通信通道
            printf("0.Waiting for client to connect.\n\r");
            communication_socket = accept(listen_socket, (struct sockaddr *)&client_addr, (socklen_t *)&length);
            if (communication_socket < 0)
            {
                perror("\n");
                printf("Server Accept Failed!\n");
                continue;
            }
            printf("New Client %d Accept...\n", communication_socket);
        }
        ClearTmpFlag();

    NET_SESSION:
        if (upgrade_step_flag == STEP_STOP)
        {
            if (fp)
            {
                fclose(fp);
                fp = NULL;
                received_length = 0;
            }

            if (communication_socket != -1)
            {
                printf("Close communication_socket......\n");
                send(communication_socket, UPGRADE_ERROR, strlen(UPGRADE_ERROR), 0);
            }
            else
            {
                continue;
            }
        }

        length = SelectRecvSocket(communication_socket, buffer);
        if (length < 0)
        {
            goto NET_SESSION;
        }
        else if (length == 0)
        {
            printf("Client %d exit!\n", communication_socket);
            close(communication_socket);
            communication_socket = -1;
            upgrade_step_flag = STEP_STOP;
            goto NET_SESSION;
        }

        if (strncmp(buffer, UPGRADE_CANCEL_FLAG, strlen(UPGRADE_CANCEL_FLAG)) == 0)
        {
            upgrade_step_flag = STEP_STOP;
        }
        else if (upgrade_step_flag == STEP_START)
        {
            /* 1.校验升级包版本信息 */
            printf("1.Verify the upgrade package version —— ");
            int ret = VerifyUpgradeVersion(buffer);
            if (ret == 1)
            {
                upgrade_step_flag = STEP_VERIFY_VERSION;
                memset(update_file_name, 0, sizeof(update_file_name));
                sprintf(update_file_name, "%s%s", UPGRADE_TMP_PATH, buffer);
                if (send(communication_socket, VERIFY_VERSION_PASS, strlen(VERIFY_VERSION_PASS), 0) < 0)
                {
                    printf("\t Socket :%d Send cmd %s fail.\n\r", communication_socket, VERIFY_VERSION_PASS);
                }
                else
                {
                    printf("Pass!!!\n\r");
                }
            }
            else if (ret == 0)
            {
                if (send(communication_socket, VERIFY_VERSION_CONSISTENT, strlen(VERIFY_VERSION_CONSISTENT), 0) < 0)
                {
                    printf("\t Socket :%d Send cmd %s fail.\n\r", communication_socket, VERIFY_VERSION_CONSISTENT);
                }
                printf("Not Pass!!! Version Consistent,Receive Data:%s\n\r", buffer);
            }
            else
            {
                upgrade_step_flag = STEP_STOP;
                printf("Not Pass!!! Version Inconformity,Receive Data:%s\n\r", buffer);
            }
        }
        else if (upgrade_step_flag == STEP_VERIFY_VERSION)
        {
            /* 2.获取升级包大小 */
            printf("2.Obtain the upgrade package size —— ");
            if (strncmp(buffer, FILE_SIZE_FLAG, strlen(FILE_SIZE_FLAG)) == 0)
            {
                char *p = strchr(buffer, ':') + 1;
                char *stop_str;
                update_file_size = strtol(p, &stop_str, 0);
                upgrade_step_flag = STEP_START_UPGRADE;
                printf("%lu Byte.\n\r", update_file_size);
                if (send(communication_socket, START_UPGRADE, strlen(START_UPGRADE), 0) < 0)
                {
                    printf("\t Send cmd %s fail.\n\r", START_UPGRADE);
                }
            }
            else
            {
                upgrade_step_flag = STEP_STOP;
                printf("Fail!!!\n\r");
            }
        }
        else if (upgrade_step_flag == STEP_START_UPGRADE)
        {
            if (fp == NULL)
            {
                fp = fopen(update_file_name, "w+");
                printf("3.The system starts to receive the written %s...\n\r", update_file_name);
            }
            if (fp != NULL)
            {
                int write_length = fwrite(buffer, sizeof(char), length, fp);
                if (write_length < length)
                {
                    printf("\t File: %s Write Error!\n", update_file_name);
                    upgrade_step_flag = STEP_STOP;
                }
                else
                {
                    received_length += length;
                    if (received_length == update_file_size)
                    {
                        fclose(fp);
                        fp = NULL;
                        received_length = 0;
                        upgrade_step_flag = STEP_WAIT_REMOTE_FILE_INTEGRITY_CHECK;
                        printf("3.Receive File: %s Finished!\n", update_file_name);
                        if (StartUpgradeApp(update_file_name))
                        {
                            if (communication_socket != -1)
                            {
                                printf("Upgrade App Succeed!!!!!\n");
                                if (send(communication_socket, UPGRADE_FINISH, strlen(UPGRADE_FINISH), 0) < 0)
                                {
                                    printf("\t Send cmd %s fail.\n\r", UPGRADE_FINISH);
                                }
                            }
                            break;
                        }
                        else
                        {
                            upgrade_step_flag = STEP_STOP;
                        }
                    }
                }
            }
            else
            {
                printf("open %s upgrade file fail.....\n\r", update_file_name);
                upgrade_step_flag = STEP_STOP;
            }
        }

        goto NET_SESSION;
    }
    if (communication_socket != -1)
    {
        close(communication_socket);
    }
    close(listen_socket);

    /* 当调用 system 函数时，它会创建一个新的进程来执行指定的命令，并且在新进程中执行 /bin/sh -c 来解释这个命令。
    这个新进程是通过 fork 系统调用来创建的，因此它会复制父进程的内存映像（包括代码段、数据段等），但是会有一些细微的差异：
    子进程会继承父进程的文件描述符：这包括打开的文件描述符、套接字描述符等。
    因此，如果在父进程中有一个TCP连接的套接字，子进程会继承这个套接字描述符。
    子进程会继承父进程的环境变量：这意味着子进程也会继承父进程的网络连接、环境变量等。
    因此，如果在父进程中建立了一个TCP连接的套接字，在调用 system 后，子进程会继承这个套接字，
    但是需要注意的是，在子进程中，这个套接字描述符的使用可能会与父进程中不同，因为套接字描述符是相对于进程的，而不是全局的。 */
    StartExeUpgrade();
    return 0;
}

static pthread_t upgrade_server_thread_id;
static bool upgrade_server_thread_run = false;

bool CreateUpgradeServerTask(void)
{
    if (upgrade_server_thread_run)
    {
        return false;
    }
    upgrade_server_thread_run = true;
    pthread_create(&upgrade_server_thread_id, NULL, UpgradeServerTask, &upgrade_server_thread_run);
    return true;
}

int Eth0Init(void)
{
#define HOUSE_SWITCH_GPIO 26
    char cmd[128] = {0};
    GPIO_LEVEL Level = GPIO_LEVEL_UNKNOWN;
    if (GpioOpen(HOUSE_SWITCH_GPIO, GPIO_DIR_IN, true) == false)
    {
        printf("Gpio %d Open Fail!!!!!\n", HOUSE_SWITCH_GPIO);
        goto Exit;
    }
    GpioLevelGet(HOUSE_SWITCH_GPIO, &Level);
    GpioEdge(HOUSE_SWITCH_GPIO, Level == GPIO_LEVEL_LOW ? RISING_EDGE : FALLING_EDGE);
Exit:
    memset(cmd, 0, sizeof(cmd));
    sprintf(cmd, "ifconfig eth0 192.168.37.%d netmask 255.255.255.0", Level == GPIO_LEVEL_LOW ? 8 : 7);
    system(cmd);
    return 0;
}

int main(int argc, char **argv)
{
    printf("==========[%s]==========\n\r", IPC_MODEL);
    printf("UpdateTime:%s%s\n\r", __DATE__, __TIME__);
    printf("==============================\n\r");

    if (access(LOCAL_VERSION_PATH "daemon.sh", F_OK) == 0)
    {
        system(LOCAL_VERSION_PATH "/daemon.sh " LOCAL_VERSION_PATH " &");
    }
    Eth0Init();
    CreateUpgradeServerTask();
    while (1)
    {
        usleep(1000);
    }
}