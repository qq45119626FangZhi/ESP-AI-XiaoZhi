#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* ==========================================================================
 * SNTP 校时 + 北京时间显示（新增，独立模块）
 *
 * 设计约束（不与既有链路耦合）：
 *   - 只在 WiFi 拿到 IP 之后调用一次 xiaozhi_time_start()；
 *   - 同步成功后立即停止 SNTP，"校验一次即可"，不做周期轮询；
 *   - 时区固定 CST-8（北京时间，无夏令时）；
 *   - 同步成功后每秒刷新一次屏幕标题栏的 "YYYY-MM-DD HH:MM:SS"；
 *   - 未同步时时间为 "--"，绝不在屏幕上显示 1970 年；
 *   - XIAOZHI_TIME_ENABLE=0 时所有接口为空实现（不建任务、不碰 lwIP）。
 * ========================================================================== */

/* 总开关 */
#define XIAOZHI_TIME_ENABLE             1

/* NTP 服务器（国内可用性优先；只配 1 个，对应 CONFIG_LWIP_SNTP_MAX_SERVERS=1） */
#define XIAOZHI_TIME_NTP_SERVER         "ntp.aliyun.com"

/* 时区：CST-8 = 东八区（北京时间） */
#define XIAOZHI_TIME_TZ                 "CST-8"

/* 屏幕显示格式 */
#define XIAOZHI_TIME_FMT                "%Y-%m-%d %H:%M:%S"

/* 单次同步等待上限（ms）与最多尝试次数 */
#define XIAOZHI_TIME_SYNC_TIMEOUT_MS    15000
#define XIAOZHI_TIME_SYNC_RETRY         2

/* 秒刷新任务：core0（栈与优先级统一见 xiaozhi_common.h，栈由 xiaozhi_task_create 放 PSRAM） */
#define XIAOZHI_TIME_TASK_CORE          0

/* 启动校时（非阻塞，内部建任务；重复调用只有第一次生效） */
esp_err_t xiaozhi_time_start(void);

/* 是否已成功授时 */
bool xiaozhi_time_is_synced(void);

/* 取当前北京时间字符串 "YYYY-MM-DD HH:MM:SS"；未授时返回 false 且 buf 置空 */
bool xiaozhi_time_format(char *buf, size_t len);
