/**
 * @file    log.h
 * @brief   带颜色和函数名的分级日志宏
 *
 * 用法：
 *   在每个 .c 文件顶部（#include 之前）定义模块标签，然后包含此头文件：
 *  
    ┌─────────────────┬──────┬────────────────────────────┐
    │       宏        │ 颜色 │          适用场景          │
    ├─────────────────┼──────┼────────────────────────────┤
    │ LOG_D(fmt, ...) │ 灰色 │ 调试细节（收包、状态跳转） │
    ├─────────────────┼──────┼────────────────────────────┤
    │ LOG_I(fmt, ...) │ 绿色 │ 正常流程节点               │
    ├─────────────────┼──────┼────────────────────────────┤
    │ LOG_W(fmt, ...) │ 黄色 │ 非致命异常                 │
    ├─────────────────┼──────┼────────────────────────────┤
    │ LOG_E(fmt, ...) │ 红色 │ 错误/初始化失败            │
    └─────────────────┴──────┴────────────────────────────┘
 *     #define LOG_TAG "NetMgr"
 *     #include "log.h"
 *
 * 打印开关:
 * #ifndef LOG_ENABLE
 *   之后直接使用：
 *     LOG_D("收到数据 len=%d", n);
 *     LOG_I("session start fd=%d", cfd);
 *     LOG_W("包长异常 len=%d", len);
 *     LOG_E("socket 创建失败");
 *
 * 输出格式（示例）：
 *   \033[32m[I][NetMgr] handle_packet\033[0m: cmd=0x56 ok
 *
 * 编译期裁剪：
 *   -DLOG_LEVEL=2  → 只保留 WARN / ERROR，去掉 DEBUG / INFO
 *   -DLOG_LEVEL=4  → 关闭全部日志（量产可用）
 *
 * 默认 LOG_TAG：
 *   若 .c 文件未定义 LOG_TAG，输出中标签为空字符串。
 */
#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>

/* =========================================================
 *  日志级别（全局默认级别）
 * ========================================================= */
#define APP_LOG_DEBUG    0
#define APP_LOG_INFO     1
#define APP_LOG_WARN     2
#define APP_LOG_ERROR    3
#define APP_LOG_NONE     4

#ifndef APP_LOG_LEVEL
#define APP_LOG_LEVEL    APP_LOG_DEBUG
#endif

// #if APP_LOG_LEVEL <= APP_LOG_DEBUG
// # define EVENT_BUS_DEBUG 1
// #else
// # undef  EVENT_BUS_DEBUG 0
// #endif

/* =========================================================
 *  ANSI 颜色码
 * ========================================================= */
#define _LC_RESET   "\033[0m"
#define _LC_GRAY    "\033[2;37m"
#define _LC_GREEN   "\033[32m"
#define _LC_YELLOW  "\033[33m"
#define _LC_RED     "\033[31m"
#define _LC_CYAN    "\033[36m"

/* =========================================================
 *  模块默认配置
 * ========================================================= */
#ifndef LOG_TAG
#define LOG_TAG ""
#endif

/* 若文件未定义 LOG_ENABLE，默认开启 */
#ifndef LOG_ENABLE
#define LOG_ENABLE 1
#endif

/* =========================================================
 *  核心输出宏
 * ========================================================= */
#if LOG_ENABLE == 1
#define _LOG_PRINT(color, lvl_str, fmt, ...)                        \
    printf(color "[" lvl_str "][" LOG_TAG "] "                      \
           _LC_CYAN "%s" _LC_RESET ": " fmt "\n",                   \
           __func__, ##__VA_ARGS__)
#else
// 模块关闭 → 所有日志都为空
#define _LOG_PRINT(color, lvl_str, fmt, ...) do {} while (0)
#endif

/* =========================================================
 *  公开宏
 * ========================================================= */
#if APP_LOG_LEVEL <= APP_LOG_DEBUG
#define LOG_D(fmt, ...)  _LOG_PRINT(_LC_GRAY,   "D", fmt, ##__VA_ARGS__)
#else
#define LOG_D(fmt, ...)  do {} while (0)
#endif

#if APP_LOG_LEVEL <= APP_LOG_INFO
#define LOG_I(fmt, ...)  _LOG_PRINT(_LC_GREEN,  "I", fmt, ##__VA_ARGS__)
#else
#define LOG_I(fmt, ...)  do {} while (0)
#endif

#if APP_LOG_LEVEL <= APP_LOG_WARN
#define LOG_W(fmt, ...)  _LOG_PRINT(_LC_YELLOW, "W", fmt, ##__VA_ARGS__)
#else
#define LOG_W(fmt, ...)  do {} while (0)
#endif

#if APP_LOG_LEVEL <= APP_LOG_ERROR
#define LOG_E(fmt, ...)  _LOG_PRINT(_LC_RED,    "E", fmt, ##__VA_ARGS__)
#else
#define LOG_E(fmt, ...)  do {} while (0)
#endif

#endif