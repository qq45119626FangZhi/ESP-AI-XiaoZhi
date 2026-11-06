#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ==========================================================================
 * M6 局域网 Web（esp_http_server）
 *   手机/电脑浏览器直接访问板子的 IP，即可：
 *     GET /                  首页：当前模式、录像状态、SD 容量 + 实时预览 + 目录入口
 *     GET /list?dir=DCIM/..  浏览 SD 卡目录（DCIM / LOG / CONFIG）
 *     GET /download?path=..  下载文件（录像 .avi / 抓拍 .jpg / 事件日志 .csv）
 *     GET /stream            MJPEG 实时预览（multipart/x-mixed-replace，≥10fps）
 *     GET /api/status        状态 JSON（给网页/上位机轮询用）
 *     GET /api/mode?set=..   远程切模式（voice/doorbell/monitor/next，302 回首页）
 *     GET /api/list?dir=..   目录 JSON（给安卓 App 用，先目录后文件）
 *
 * 设计要点
 *   1. 视频帧**不再单独 grab 摄像头**：由 xiaozhi_camview 预览任务把同一帧 JPEG
 *      通过 set_frame_tap 分发过来（槽位 XIAOZHI_CAMVIEW_TAP_STREAM），
 *      因此远程预览不会把本机 10fps 的预览拖慢。
 *   2. 只有监控模式（预览任务在跑、摄像头已启动）才有视频源；其它模式
 *      /stream 返回 503 并提示"请先切到监控模式"，不抢语音链路的资源。
 *   3. /stream 用 httpd_req_async_handler_begin 交给独立任务，避免长连接
 *      独占 httpd 任务导致列表/下载被卡住（同一时刻只服务 1 个预览客户端）。
 *   4. 路径参数只接受 /sdcard 下的相对路径，拒绝 ".." 与绝对路径。
 *   5. 大文件下载按 4KB 分块 chunked 发送，不占大块内存。
 *
 * XIAOZHI_HTTP_ENABLE=0 时为空实现（不起服务、不申请缓冲）。
 * ========================================================================== */

/* 总开关 */
#define XIAOZHI_HTTP_ENABLE          1

/* 监听端口（80 便于手机直接输 IP 访问） */
#define XIAOZHI_HTTP_PORT            80

/* 一次 MJPEG 传输的单帧上限（PSRAM 双缓冲，2 × 该值） */
#define XIAOZHI_HTTP_FRAME_MAX       (64 * 1024)

/* httpd 任务栈（内部 RAM；要跑 HTML 拼接 + FATFS 读，别太小）。
 * 实机（2026-11-06）在 SR/音频初始化之后，内部 RAM 最大连续块仅 ~7KB，
 * 因此这里只作为“期望上限”，实际栈由 xiaozhi_http_init() 按剩余最大块裁剪，
 * 下限 XIAOZHI_HTTP_STACK_MIN（IDF 默认 4096）。 */
#define XIAOZHI_HTTP_STACK           8192
#define XIAOZHI_HTTP_STACK_MIN       4096

/* 同时允许的 TCP 连接数（1 个预览 + 列表/下载） */
#define XIAOZHI_HTTP_MAX_SOCKETS     4

/* 下载分块大小 */
#define XIAOZHI_HTTP_TX_CHUNK        4096

/* MJPEG 发送任务：core0（栈与优先级统一见 xiaozhi_common.h 的
 * xiaozhi_task_stack_t / xiaozhi_task_prio_t，栈由 xiaozhi_task_create 放 PSRAM） */
#define XIAOZHI_HTTP_STREAM_CORE     0

/* 初始化（WiFi 拿到 IP 之后调用一次；重复调用幂等）。
 * 失败不影响语音/录像等其它功能。 */
esp_err_t xiaozhi_http_init(void);

/* 当前访问地址（"http://192.168.x.x/"，未拿到 IP 时返回 ""） */
const char *xiaozhi_http_url(void);

/* 是否有客户端正在拉 MJPEG 流 */
bool xiaozhi_http_stream_active(void);
