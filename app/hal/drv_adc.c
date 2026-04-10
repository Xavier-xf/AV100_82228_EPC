/**
 * @file    drv_adc.c
 * @brief   ADC 采集驱动
 *
 *   注册式回调 DrvAdcSetCallback，并直接通过 sysfs 读取电压值。
 *
 *   sysfs 路径：/sys/bus/iio/devices/iio:device0/in_voltage0_raw
 *   内核模块：  /usr/modules/ak_saradc.ko
 *   采样间隔：  50ms
 */
#define LOG_TAG "DrvAdc"
#include "log.h"

#include "drv_adc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <assert.h>

#define AK_SARADC_PATH   "/usr/modules/ak_saradc.ko"
#define SARADC_DEV       "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define DEFAULT_VOLTAGE  3299   /* 无按键时高电平参考值 */
#define SAMPLE_INTERVAL_MS 50

typedef struct {
    pthread_mutex_t lock;
    DrvAdcCallback  callback;
    int             running;
} DrvAdcCtx;

static DrvAdcCtx s_adc = {
    .lock     = PTHREAD_MUTEX_INITIALIZER,
    .callback = NULL,
    .running  = 0,
};

void DrvAdcSetCallback(DrvAdcCallback cb)
{
    pthread_mutex_lock(&s_adc.lock);
    s_adc.callback = cb;
    pthread_mutex_unlock(&s_adc.lock);
}

static void *adc_thread(void *arg)
{
    (void)arg;
    /* 确保 sysfs 节点存在 */
    assert(access(SARADC_DEV, F_OK) == 0);

    int fd = open(SARADC_DEV, O_RDONLY);
    if (fd < 0) { LOG_E("open %s fail", SARADC_DEV); return NULL; }

    char raw[64];

    while (1) {
        pthread_mutex_lock(&s_adc.lock);
        int running = s_adc.running;
        DrvAdcCallback cb = s_adc.callback;
        pthread_mutex_unlock(&s_adc.lock);

        if (!running) break;

        lseek(fd, 0, SEEK_SET);
        memset(raw, 0, sizeof(raw));
        if (read(fd, raw, sizeof(raw)) > 0) {
            int voltage = atoi(raw);
            if (cb) cb(voltage);
        }
        usleep((unsigned int)(SAMPLE_INTERVAL_MS * 1000));
    }
    close(fd);
    LOG_I("thread exit");
    return NULL;
}

int DrvAdcInit(void)
{
    /* 加载内核模块 */
    assert(access(AK_SARADC_PATH, F_OK) == 0);
    system("insmod " AK_SARADC_PATH);
    usleep(50 * 1000);   /* 等待 sysfs 节点创建 */

    pthread_mutex_lock(&s_adc.lock);
    if (s_adc.running) { pthread_mutex_unlock(&s_adc.lock); return 0; }
    s_adc.running = 1;
    pthread_mutex_unlock(&s_adc.lock);

    pthread_t tid;
    if (pthread_create(&tid, NULL, adc_thread, NULL) != 0) {
        pthread_mutex_lock(&s_adc.lock); s_adc.running = 0; pthread_mutex_unlock(&s_adc.lock);
        LOG_E("thread create failed");
        return -1;
    }
    pthread_detach(tid);
    LOG_I("init ok, dev=%s", SARADC_DEV);
    return 0;
}

int DrvAdcDeinit(void)
{
    pthread_mutex_lock(&s_adc.lock); s_adc.running = 0; pthread_mutex_unlock(&s_adc.lock);
    return 0;
}
