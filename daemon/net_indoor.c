/*client.c*/
#include <netinet/in.h> // for sockaddr_in
#include <sys/socket.h> // for socket
#include <sys/select.h>
#include <stdio.h>  // for printf
#include <stdlib.h> // for exit
#include <string.h> // for bzero
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include "pthread.h"
#define FILE_MAX_LEN 256

#define HELLO_WORLD_SERVER_PORT 6666
#define BUFFER_SIZE 1024
#define FILE_NAME_MAX_SIZE 512

#define FILE_SIZE_FLAG "file_size:"
#define UPGRADE_ERROR "Upgrade Error"
#define UPGRADE_FINISH "Upgrade Finish"
#define VERIFY_VERSION_PASS "Version Verification Passed"
#define START_UPGRADE "Start Upgrade"

#ifndef IPC_MODEL
#define IPC_MODEL "SAT_CAMERA"
#endif
#ifndef LOCAL_VERSION_PATH
#define LOCAL_VERSION_PATH "/mnt/tf/"
#endif
static char *FindLocalUpgrade(void)
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

static int FileSizeGet(FILE *fp)
{
    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    return file_size;
}

static void PrintfProgress(int percentage)
{
    printf("\r%d%%", percentage);
    fflush(stdout);
}

int CreateUpgradeTcpClient(char *server_ip)
{
    // 设置一个socket地址结构client_addr, 代表客户机的internet地址和端口
    struct sockaddr_in client_addr;
    bzero(&client_addr, sizeof(client_addr));
    client_addr.sin_family = AF_INET;                // internet协议族
    client_addr.sin_addr.s_addr = htons(INADDR_ANY); // INADDR_ANY表示自动获取本机地址
    client_addr.sin_port = htons(0);                 // auto allocated, 让系统自动分配一个空闲端口

    // 创建用于internet的流协议(TCP)类型socket，用client_socket代表客户端socket
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0)
    {
        printf("Create Socket Failed!\n");
        return -1;
    }

    // 把客户端的socket和客户端的socket地址结构绑定
    if (bind(client_socket, (struct sockaddr *)&client_addr, sizeof(client_addr)))
    {
        printf("Client Bind Port Failed!\n");
        return -1;
    }

    // 设置一个socket地址结构server_addr,代表服务器的internet地址和端口
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;

    // 服务器的IP地址来自程序的参数
    if (inet_aton(server_ip, &server_addr.sin_addr) == 0)
    {
        printf("Server IP Address Error!\n");
        return -1;
    }

    server_addr.sin_port = htons(HELLO_WORLD_SERVER_PORT);
    socklen_t server_addr_length = sizeof(server_addr);

    // 向服务器发起连接请求，连接成功后client_socket代表客户端和服务器端的一个socket连接
    if (connect(client_socket, (struct sockaddr *)&server_addr, server_addr_length) < 0)
    {
        printf("Can Not Connect To %s!\n", (char *)server_ip);
        return -1;
    }
    return client_socket;
}

int SelectRecvSocket(int socket, char *buffer)
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

    return recv(socket, buffer, BUFFER_SIZE, 0);
}

#define STEP_START 0
#define STEP_VERIFY_VERSION 1
#define STEP_UPGRADE_SIZE 2
#define STEP_START_UPGRADE 3
#define STEP_WAIT_REMOTE_FILE_INTEGRITY_CHECK 4
#define STEP_STOP 5

static int upgrade_step_flag = STEP_START;
static pthread_t upgrade_client_thread_id;
static bool upgrade_client_thread_run = false;

void ClearArpCache(char *ip)
{
    char str[128] = {0};
    memset(str, 0, sizeof(str));
    sprintf(str, "arp -d %s", ip);
    system(str);
}

bool CheckUpgradeError(char *buffer)
{
    if (strncmp(buffer, UPGRADE_ERROR, strlen(UPGRADE_ERROR)) == 0)
    {
        printf("Upgrade package Error!!! exit .....\n");
        return true;
    }
    return false;
}
bool CheckUpgradeFinish(char *buffer)
{
    if (strncmp(buffer, UPGRADE_FINISH, strlen(UPGRADE_FINISH)) == 0)
    {
        return true;
    }
    return false;
}

static void *CreateClientTask(void *arg)
{
    char *upgrade_server_ip = (char *)arg;
    if (upgrade_server_ip == NULL)
    {
        printf("Upgrade server ip is NULL.\n");
        return NULL;
    }

    /* 避免ARP缓存中的MAC地址因门口机id切换与之前IP不相符，每次连接前清除ARP缓存 */
    ClearArpCache(upgrade_server_ip);

    int Length = 0;
    int file_size = 0;
    FILE *upgrade_fp = NULL;
    int client_socket = CreateUpgradeTcpClient(upgrade_server_ip);

    /* 1.向服务器发送当前软件版本型号 */
    char *local_upgrade;
    if ((local_upgrade = FindLocalUpgrade()))
    {
        printf("1.Check Local Upgrade Version %s\n", local_upgrade);
        char path[128] = {0};
        memset(path, 0, sizeof(path));
        sprintf(path, "%s%s", LOCAL_VERSION_PATH, local_upgrade);
        if ((upgrade_fp = fopen(path, "r")) == NULL)
        {
            printf("Open Upgrade package fail.....\n");
            goto CLOSE_SOCKET;
        }
        if (send(client_socket, local_upgrade, strlen(local_upgrade) + 1, 0) < 0)
        {
            printf("\t Send local_upgrade %s fail.\n\r", local_upgrade);
            goto CLOSE_FP;
        }
    }
    else
    {
        printf("Upgrade package not found.....\n");
        goto CLOSE_SOCKET;
    }

    upgrade_step_flag = STEP_VERIFY_VERSION;
    while (1)
    {
        char buffer[BUFFER_SIZE];
        bzero(buffer, sizeof(buffer));
        if ((Length = SelectRecvSocket(client_socket, buffer)) == 0)
        {
            goto CLOSE_FP;
        }

        if (Length > 0)
        {
            if (CheckUpgradeError(buffer))
            {
                goto CLOSE_FP;
            }

            switch (upgrade_step_flag)
            {
            case STEP_VERIFY_VERSION:
            {
                printf("2.Verify Local Upgrade Version —— ");
                if (strncmp(buffer, VERIFY_VERSION_PASS, strlen(VERIFY_VERSION_PASS)) == 0)
                {
                    printf("Succeed!!!\n");
                    file_size = FileSizeGet(upgrade_fp);
                    bzero(buffer, sizeof(buffer));
                    sprintf(buffer, "%s%d", FILE_SIZE_FLAG, file_size);
                    if (send(client_socket, buffer, BUFFER_SIZE, 0) < 0)
                    {
                        printf("Send Fail!!! File size %d Byte.\n\r", file_size);
                        goto CLOSE_FP;
                    }
                    else
                    {
                        upgrade_step_flag = STEP_UPGRADE_SIZE;
                        printf("Send Succeed !!! File size %d Byte.\n\r", file_size);
                    }
                }
                else
                {
                    printf("Fail!!!\n");
                    goto CLOSE_FP;
                }
            }
            break;
            case STEP_UPGRADE_SIZE:
            case STEP_START_UPGRADE:
            {
                printf("3.Send Upgrade Package Size ... ");
                if (strncmp(buffer, START_UPGRADE, strlen(START_UPGRADE)) == 0)
                {
                    upgrade_step_flag = STEP_START_UPGRADE;
                    printf("\n4.Start Upgrade ... \n");
                    bzero(buffer, BUFFER_SIZE);
                    int file_block_length = 0;
                    int has_send_size = 0;
                    while ((file_block_length = fread(buffer, sizeof(char), BUFFER_SIZE, upgrade_fp)) > 0)
                    {
                        // printf("\t Has been sent = %d byte.\n", file_block_length);
                        //  发送buffer中的字符串到new_server_socket,实际上就是发送给客户端
                        if (send(client_socket, buffer, file_block_length, 0) < 0)
                        {
                            printf("\n\t Send File:\t%s Failed!\n", local_upgrade);
                            goto CLOSE_FP;
                            break;
                        }
                        else
                        {
                            has_send_size += file_block_length;
                            PrintfProgress(has_send_size * 100 / file_size);
                        }
                        bzero(buffer, sizeof(buffer));
                    }
                    upgrade_step_flag = STEP_WAIT_REMOTE_FILE_INTEGRITY_CHECK;
                    printf("\r\t File:\t%s Transfer Finished!\n", local_upgrade);
                }
                else
                {
                    printf("Fail!!!\n");
                    goto CLOSE_FP;
                }
            }
            break;
                break;
            case STEP_WAIT_REMOTE_FILE_INTEGRITY_CHECK:
            {
                printf("5.Return Upgrade Result ——");
                if (CheckUpgradeFinish(buffer))
                {
                    printf("Succeed!!!\n");
                }
                else
                {
                    printf("Return :%s Fail!!!\n", buffer);
                }
                goto CLOSE_FP;
            }
            break;

            default:
                break;
            }
        }
    }

CLOSE_FP:
    fclose(upgrade_fp);
CLOSE_SOCKET:
    close(client_socket);
    upgrade_client_thread_run = false;
    exit(0);
    return /* (void *)&ret */ NULL;
}

bool CreateUpgradeClientTask(char *server_ip)
{
    if (upgrade_client_thread_run)
    {
        return false;
    }
    upgrade_client_thread_run = true;
    int ret = pthread_create(&upgrade_client_thread_id, NULL, CreateClientTask, (void *)server_ip);
    pthread_detach(upgrade_client_thread_id);
    if (ret == -1)
    {
        upgrade_client_thread_run = false;
        return false;
    }
    return true;
}
bool upgrade_client_status(void)
{
    return upgrade_client_thread_run;
}

bool destroy_upgrade_client_task(void)
{
    if (upgrade_client_thread_run == false)
    {
        return false;
    }
    upgrade_client_thread_run = false;
    printf("CreateClientTask ===============>over!!!!!\n\r");
    upgrade_client_thread_id = 0;
    return true;
}

int main(int argc, char **argv)
{
    CreateUpgradeClientTask("192.168.37.8");
    while (1)
    {
        usleep(1000);
    }
}