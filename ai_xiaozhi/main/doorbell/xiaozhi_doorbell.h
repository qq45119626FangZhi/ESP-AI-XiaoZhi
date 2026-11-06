#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ==========================================================================
 * 门铃业务（M4 的一部分，不含摄像头本身）
 *   1. 触发入口：SW2 长按（main.c）+ 语音 `Mode.SetMode=doorbell`。
 *      xiaozhi_appmode 切到 XIAOZHI_MODE_DOORBELL 时回调 on_enter，
 *      已在该模式时再次长按回调 on_trigger（= 重复按铃）。
 *   2. 本模块负责：提示音（现场合成 PCM 双音，不依赖音频文件）、
 *      屏幕提示（文本 + emoji）、事件编号、SD 事件日志（/LOG/event.csv）、
 *      云端事件上报，以及"开始录像"的钩子调度。
 *   3. 录像后端通过 xiaozhi_doorbell_set_recorder() 注入（M2 摄像头 +
 *      M5 MJPEG-AVI 录像模块完成后注册）；未注册时只打印 WARN 并跳过，
 *      不会阻塞响铃/日志，也不影响既有语音链路。
 *   4. 全部实现由 XIAOZHI_DOORBELL_ENABLE 控制，=0 时退化为空实现。
 * ========================================================================== */

/* ---------------- 编译期开关 ---------------- */

/* 门铃模块总开关：0 = 不注册模式钩子、不创建任务（等同未接入） */
#define XIAOZHI_DOORBELL_ENABLE 1

/* 1 = 响铃（现场合成"叮咚"双音 PCM 经 ES8311 播放） */
#define XIAOZHI_DOORBELL_RING_ENABLE 1

/* 1 = 响铃期间屏蔽 VAD（防止喇叭声音被 MIC 拾取造成自激/误唤醒） */
#define XIAOZHI_DOORBELL_RING_VAD_GATE 1

/* 1 = 向云端上报门铃事件（ws 未连接时自动跳过） */
#define XIAOZHI_DOORBELL_WS_NOTIFY 1

/* 1 = 每次事件追加一行到 /sdcard/LOG/event.csv（SD 未挂载时跳过）
 * 当前置 0：调试三模切换期间不向卡写任何数据，改为只打串口日志 */
#define XIAOZHI_DOORBELL_LOG_ENABLE 0

/* 事件录像时长（ms）；M4 规划值 20s */
#define XIAOZHI_DOORBELL_RECORD_MS 20000

/* ---------------- 录像后端接口（由 M2/M5 注入） ---------------- */

typedef struct {
    /* 开始录像：duration_ms 到时后本模块会调用 stop()；返回 ESP_OK 才算成功 */
    esp_err_t (*start)(const char *event_id, uint32_t duration_ms, void *ctx);
    /* 停止录像（可能被模式切换提前调用，必须可重入、可空实现） */
    void (*stop)(void *ctx);
    void *ctx;
} xiaozhi_doorbell_recorder_t;

/* ---------------- 对外 API ---------------- */

/* 初始化：创建事件队列与门铃任务，并注册门铃模式的 enter/exit/trigger 钩子。
 * 须在 xiaozhi_appmode_init() 之后、音频初始化之后调用一次。 */
void xiaozhi_doorbell_init(void);

/* 注册录像后端（可在任意时刻调用，传 NULL 表示注销） */
esp_err_t xiaozhi_doorbell_set_recorder(const xiaozhi_doorbell_recorder_t *recorder);

/* 状态查询 */
bool xiaozhi_doorbell_is_ringing(void);   /* 正在播放提示音（VAD 门控用） */
bool xiaozhi_doorbell_is_recording(void); /* 事件录像进行中 */
uint32_t xiaozhi_doorbell_event_count(void);
const char *xiaozhi_doorbell_last_event_id(void);
