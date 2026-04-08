/**
 * @file    drv_keypad.c
 * @brief   XW12A 数字键盘 HAL 驱动
 *
 * 原始字节 → 按键值映射：
 *   0x0A→0  0x01→1  0x02→2  0x03→3  0x04→4  0x05→5
 *   0x06→6  0x10→7  0x08→8  0x07→9  0x09→*  0x0B→#
 */
#define LOG_TAG "Keypad"
#include "log.h"

#include "drv_keypad.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#define XW12A_DRV_PATH         "/usr/modules/XW12A.ko"
#define SIMULATED_I2C_DRV_PATH "/usr/modules/sim_idrv.ko"
#define SIMULAT_I2C_DEV_PATH   "/dev/SIMULAT_I2C"
#define XW12A_DEV_PATH         "/dev/XW12A"
#define XW12A_KEY_LEN          1
#define POLL_INTERVAL_US       (10 * 1000)

/* 原始字节 → 按键值查表 */
#define KEY_TABLE_SIZE  0x11
static const signed char s_key_table[KEY_TABLE_SIZE] = {
    [0x0A] = KEYPAD_KEY_0,
    [0x01] = KEYPAD_KEY_1,
    [0x02] = KEYPAD_KEY_2,
    [0x03] = KEYPAD_KEY_3,
    [0x04] = KEYPAD_KEY_4,
    [0x05] = KEYPAD_KEY_5,
    [0x06] = KEYPAD_KEY_6,
    [0x10] = KEYPAD_KEY_7,
    [0x08] = KEYPAD_KEY_8,
    [0x07] = KEYPAD_KEY_9,
    [0x09] = KEYPAD_KEY_STAR,
    [0x0B] = KEYPAD_KEY_POUND,
};

static const unsigned char s_key_valid[KEY_TABLE_SIZE] = {
    [0x0A] = 1, [0x01] = 1, [0x02] = 1, [0x03] = 1,
    [0x04] = 1, [0x05] = 1, [0x06] = 1, [0x10] = 1,
    [0x08] = 1, [0x07] = 1, [0x09] = 1, [0x0B] = 1,
};

static DrvKeypadCallback s_callback = NULL;
static int               s_fd       = -1;
static int               s_running  = 0;
static pthread_mutex_t   s_lock     = PTHREAD_MUTEX_INITIALIZER;

void DrvKeypadSetCallback(DrvKeypadCallback cb)
{
    pthread_mutex_lock(&s_lock);
    s_callback = cb;
    pthread_mutex_unlock(&s_lock);
}

static void *keypad_poll_thread(void *arg)
{
    (void)arg;
    unsigned char raw;

    while (1) {
        pthread_mutex_lock(&s_lock);
        int fd      = s_fd;
        int running = s_running;
        DrvKeypadCallback cb = s_callback;
        pthread_mutex_unlock(&s_lock);

        if (fd < 0 || !running) break;

        if (read(fd, &raw, XW12A_KEY_LEN) == XW12A_KEY_LEN) {
            if (raw < KEY_TABLE_SIZE && s_key_valid[raw]) {
                if (cb) cb((int)s_key_table[raw]);
            } else {
                LOG_W("unknown raw key=0x%02X", raw);
            }
        }

        usleep(POLL_INTERVAL_US);
    }

    LOG_I("poll thread exit");
    return NULL;
}

int DrvKeypadInit(void)
{
    if (access(XW12A_DRV_PATH, F_OK) != 0) {
        LOG_W("%s not found, keypad disabled", XW12A_DRV_PATH);
        return -1;
    }

    if (access(SIMULAT_I2C_DEV_PATH, F_OK) != 0)
        system("insmod " SIMULATED_I2C_DRV_PATH);

    if (access(XW12A_DEV_PATH, F_OK) != 0)
        system("insmod " XW12A_DRV_PATH);

    int fd = open(XW12A_DEV_PATH, O_RDWR);
    if (fd < 0) {
        LOG_E("open %s fail", XW12A_DEV_PATH);
        return -1;
    }

    pthread_mutex_lock(&s_lock);
    s_fd      = fd;
    s_running = 1;
    pthread_mutex_unlock(&s_lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, keypad_poll_thread, NULL) != 0) {
        pthread_mutex_lock(&s_lock);
        s_running = 0;
        s_fd      = -1;
        pthread_mutex_unlock(&s_lock);
        close(fd);
        LOG_E("create poll thread fail");
        return -1;
    }
    pthread_detach(tid);

    LOG_I("init ok (%s)", XW12A_DEV_PATH);
    return 0;
}
