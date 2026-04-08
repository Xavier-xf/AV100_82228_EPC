/**
 * @file    drv_card.c
 * @brief   RC522 RFID 读卡器 HAL 驱动
 *
 * 【关键设计说明】
 * RC522 驱动是阻塞字符设备，不使用内核 FIFO：
 *   - FIONREAD 始终返回 0（无数据在内核缓冲等待）
 *   - read(fd, buf, 4) 在无卡时阻塞，刷卡后立即返回 4 字节
 *
 * 因此本驱动必须在 FIONREAD 返回 0 时仍调用 read()：
 *   当 recv_size % 4 == 0（含 recv_size==0），直接调用 read()，
 *   让驱动自然阻塞直到卡片到来。
 *
 * 若加入 recv_size > 0 守卫，会导致 FIONREAD==0 时永远不调用
 * read()，线程只是空转 200ms，从不读卡。
 */
#define LOG_TAG "DrvCard"
#include "log.h"

#include "drv_card.h"
#include <sys/ioctl.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#define SIMULATED_I2C_DRV_PATH  "/usr/modules/sim_idrv.ko"
#define RC522_DRV_PATH          "/usr/modules/RC522.ko"
#define RC522_DEV_PATH          "/dev/RC522"

#define RC522_CARD_LEN          4    /* RC522 每次读取的字节数 */
#define POLL_INTERVAL_US        (200 * 1000)

#define DEVICE_RESET_CMD        0xABCD

typedef struct {
    pthread_mutex_t lock;
    DrvCardCallback callback;
    int             fd;
    int             running;
    int             recv_cache_len;  /* 上次 FIONREAD 返回值 */
} DrvCardCtx;

static DrvCardCtx s_card = {
    .lock           = PTHREAD_MUTEX_INITIALIZER,
    .callback       = NULL,
    .fd             = -1,
    .running        = 0,
    .recv_cache_len = -1,
};

void DrvCardSetCallback(DrvCardCallback cb)
{
    pthread_mutex_lock(&s_card.lock);
    s_card.callback = cb;
    pthread_mutex_unlock(&s_card.lock);
}

static int recv_buf_size(int fd)
{
    int bytes = 0;
    ioctl(fd, FIONREAD, &bytes);
    return bytes;
}

static void rc522_reset(int fd)
{
    if (ioctl(fd, DEVICE_RESET_CMD) < 0)
        LOG_W("ioctl reset fail");
}

/* ====================================================================
 *  轮询线程
 *
 *  每轮流程（recv_cache_len 初始为 -1）：
 *
 *    recv_size = FIONREAD()            // 通常为 0（驱动阻塞模式）
 *
 *    // 连续两次相同且非零 → 跳过
 *    if (recv_size > 0 && recv_size == recv_cache_len)
 *        recv_size = -1;               // 设哨兵
 *
 *    recv_cache_len = recv_size;
 *
 *    // 整倍数（含 0）→ 调用 read()
 *    // ★ recv_size==0 时 (0 % 4 == 0) 为真 → read() 阻塞等卡
 *    // ★ recv_size==-1（哨兵）→ (-1) % 4 = -1 ≠ 0 → 跳过
 *    if (recv_size % 4 == 0) {
 *        n = read(fd, buf, 4);         // 无卡时阻塞，刷卡后返回
 *        if (n == 4)  handle(buf);
 *        if (n < 0)   reset(fd);
 *    }
 *
 *    usleep(200ms);
 * ==================================================================== */
static void *card_poll_thread(void *arg)
{
    (void)arg;
    char data[RC522_CARD_LEN];
    int  running = 1;

    while (running) {
        pthread_mutex_lock(&s_card.lock);
        int fd              = s_card.fd;
        running             = s_card.running;
        DrvCardCallback cb  = s_card.callback;
        int last_size       = s_card.recv_cache_len;
        pthread_mutex_unlock(&s_card.lock);

        if (fd < 0 || !running) break;

        int recv_size = recv_buf_size(fd);

        /* 连续两次 FIONREAD 相同且非零：跳过，设哨兵 -1 */
        if (recv_size > 0 && recv_size == last_size)
            recv_size = -1;

        pthread_mutex_lock(&s_card.lock);
        s_card.recv_cache_len = recv_size;
        pthread_mutex_unlock(&s_card.lock);

        /* 整倍数（含 recv_size==0）→ 调用 read()
         *
         * ★ recv_size==0 时 (0 % 4 == 0) 条件为真，
         *   read(fd, data, 4) 会阻塞直到驱动收到卡片数据。
         *   不加 recv_size>0 守卫，才能触发这个阻塞等待行为。
         *
         * recv_size==-1（哨兵）时：(-1) % 4 = -1 ≠ 0，跳过，不读也不复位。
         */
        if (recv_size % RC522_CARD_LEN == 0) {
            memset(data, 0, sizeof(data));
            int n = read(fd, data, RC522_CARD_LEN);
            if (n == RC522_CARD_LEN) {
                if (cb) cb(data);
            } else if (n < 0) {
                /* read 真正出错 → 复位 */
                rc522_reset(fd);
            }
            /* n == 0：EOF，忽略 */
        }

        usleep(POLL_INTERVAL_US);
    }

    LOG_I("poll thread exit");
    return NULL;
}

/* =========================================================
 *  初始化
 * ========================================================= */
int DrvCardInit(void)
{
    if (access(RC522_DRV_PATH, F_OK) != 0) {
        LOG_W("%s not found, card reader disabled", RC522_DRV_PATH);
        return -1;
    }

    system("insmod " SIMULATED_I2C_DRV_PATH);
    usleep(10 * 1000);
    system("insmod " RC522_DRV_PATH);

    int fd = open(RC522_DEV_PATH, O_RDWR);
    if (fd < 0) {
        LOG_E("open %s fail", RC522_DEV_PATH);
        return -1;
    }
    LOG_I("%s opened", RC522_DEV_PATH);

    pthread_mutex_lock(&s_card.lock);
    if (s_card.running) {
        pthread_mutex_unlock(&s_card.lock);
        close(fd);
        return 0;
    }
    s_card.fd      = fd;
    s_card.running = 1;
    pthread_mutex_unlock(&s_card.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, card_poll_thread, NULL) != 0) {
        pthread_mutex_lock(&s_card.lock);
        s_card.running = 0;
        s_card.fd      = -1;
        pthread_mutex_unlock(&s_card.lock);
        close(fd);
        return -1;
    }
    pthread_detach(tid);

    LOG_I("init ok");
    return 0;
}
