/**
 * @file    app_intercom.h
 * @brief   对讲业务状态机
 */
#ifndef _APP_INTERCOM_H_
#define _APP_INTERCOM_H_

typedef enum {
    INTERCOM_STATE_IDLE       = 0,
    INTERCOM_STATE_CALLING,      /* 门铃已按，等待接听 */
    INTERCOM_STATE_TALKING,      /* 双向通话中          */
    INTERCOM_STATE_MONITORING,   /* 单向监控中          */
} IntercomState;

IntercomState AppIntercomGetState(void);
int           AppIntercomInit(void);

#endif
