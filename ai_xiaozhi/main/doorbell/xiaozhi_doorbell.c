#include "xiaozhi_doorbell.h"

#include "xiaozhi_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "xiaozhi_appmode.h"
#include "xiaozhi_audio.h"
#include "xiaozhi_lvgl.h"
#include "xiaozhi_sdcard.h"
#if XIAOZHI_DOORBELL_WS_NOTIFY
#include "xiaozhi_ws.h"
#endif

#define TAG "doorbell"

/* 任务/队列参数：门铃任务只做"响铃 + 记账 + 调录像钩子"，放 core0 避让语音链路 */
/* 栈与优先级统一见 xiaozhi_common.h 的 xiaozhi_task_stack_t / xiaozhi_task_prio_t */
#define XIAOZHI_DOORBELL_TASK_CORE  0
#define XIAOZHI_DOORBELL_QUEUE_LEN  4
#define XIAOZHI_DOORBELL_IDLE_MS    200

/* 重复触发去抖：同一次长按可能产生多帧事件，1.5s 内只处理一次 */
#define XIAOZHI_DOORBELL_DEBOUNCE_MS 1500

/* 提示音参数：16kHz / 16bit / 单声道，与 ES8311 现有采样配置一致 */
#define XIAOZHI_DOORBELL_SAMPLE_RATE 16000
#define XIAOZHI_DOORBELL_CHUNK       320 /* 20ms 一包，避免一次性阻塞 I2S 太久 */
#define XIAOZHI_DOORBELL_FADE_MS     8   /* 淡入淡出，避免起落爆音 */
#define XIAOZHI_DOORBELL_AMPLITUDE   9000

/* 门铃事件日志（与 SD 模块的目录约定一致，循环覆盖只清理 DCIM，不碰 LOG） */
#define XIAOZHI_DOORBELL_LOG_PATH "/sdcard/LOG/event.csv"

/* 界面提示文案 */
#define XIAOZHI_DOORBELL_TEXT_FIRST  "有人按门铃"
#define XIAOZHI_DOORBELL_TEXT_REPEAT "门铃再次按下"
#define XIAOZHI_DOORBELL_TEXT_IDLE   "你好，我是小智"
#define XIAOZHI_DOORBELL_EMOJI       "surprised"
#define XIAOZHI_DOORBELL_EMOJI_IDLE  "neutral"

typedef struct {
    xiaozhi_mode_src_t src;
    bool repeat; /* true = 已在门铃模式下再次按下 */
} xiaozhi_doorbell_req_t;

#if XIAOZHI_DOORBELL_ENABLE

static bool s_inited = false;
static QueueHandle_t s_queue = NULL;

static volatile bool s_ringing = false;
static volatile bool s_recording = false;
static int64_t s_record_deadline_us = 0;
static int64_t s_last_event_us = 0;

static uint32_t s_event_count = 0;
static char s_last_event_id[32] = {0};
static xiaozhi_doorbell_recorder_t s_recorder = {0};

static int16_t *s_ring_pcm = NULL;
static size_t s_ring_samples = 0;

/* -------------------------------------------------------------------------- */
/* 提示音：现场合成正弦双音（"叮" 1046Hz + "咚" 784Hz）                        */
/* -------------------------------------------------------------------------- */

static void xiaozhi_doorbell_tone(int16_t *dst, size_t *offset, uint32_t freq_hz, uint32_t ms)
{
    size_t n = (size_t)((uint64_t)XIAOZHI_DOORBELL_SAMPLE_RATE * ms / 1000ULL);
    size_t fade = (size_t)((uint64_t)XIAOZHI_DOORBELL_SAMPLE_RATE * XIAOZHI_DOORBELL_FADE_MS / 1000ULL);
    if (fade == 0) {
        fade = 1;
    }
    const float two_pi = 6.28318530718f;
    for (size_t i = 0; i < n; i++) {
        float env = 1.0f;
        if (i < fade) {
            env = (float)i / (float)fade;
        } else if (n - i <= fade) {
            env = (float)(n - i) / (float)fade;
        }
        float v = sinf(two_pi * (float)freq_hz * (float)i / (float)XIAOZHI_DOORBELL_SAMPLE_RATE);
        dst[(*offset)++] = (int16_t)(v * env * (float)XIAOZHI_DOORBELL_AMPLITUDE);
    }
}

static void xiaozhi_doorbell_silence(int16_t *dst, size_t *offset, uint32_t ms)
{
    size_t n = (size_t)((uint64_t)XIAOZHI_DOORBELL_SAMPLE_RATE * ms / 1000ULL);
    memset(dst + (*offset), 0, n * sizeof(int16_t));
    (*offset) += n;
}

/* 首次响铃时合成一次并缓存（约 740ms => 23KB，放 PSRAM） */
static esp_err_t xiaozhi_doorbell_build_ring(void)
{
    if (s_ring_pcm != NULL) {
        return ESP_OK;
    }

    const uint32_t ms_total = 260 + 60 + 420;
    size_t samples = (size_t)((uint64_t)XIAOZHI_DOORBELL_SAMPLE_RATE * ms_total / 1000ULL) + 16;

    s_ring_pcm = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring_pcm == NULL) {
        s_ring_pcm = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (s_ring_pcm == NULL) {
        ESP_LOGE(TAG, "no memory for ring tone (%u bytes)", (unsigned)(samples * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    xiaozhi_doorbell_tone(s_ring_pcm, &off, 1046, 260);  /* 叮 */
    xiaozhi_doorbell_silence(s_ring_pcm, &off, 60);
    xiaozhi_doorbell_tone(s_ring_pcm, &off, 784, 420);   /* 咚 */
    s_ring_samples = off;

    ESP_LOGI(TAG, "ring tone ready: %u samples (%u ms)", (unsigned)s_ring_samples,
             (unsigned)(s_ring_samples * 1000 / XIAOZHI_DOORBELL_SAMPLE_RATE));
    return ESP_OK;
}

static void xiaozhi_doorbell_ring(void)
{
#if XIAOZHI_DOORBELL_RING_ENABLE
    if (xiaozhi_doorbell_build_ring() != ESP_OK) {
        return;
    }

    /* s_ringing 供 VAD 门控使用：整段播放期间屏蔽 VAD，避免喇叭自激 */
    s_ringing = true;
    size_t sent = 0;
    while (sent < s_ring_samples) {
        size_t n = s_ring_samples - sent;
        if (n > XIAOZHI_DOORBELL_CHUNK) {
            n = XIAOZHI_DOORBELL_CHUNK;
        }
        xiaozhi_audio_play((uint8_t *)(s_ring_pcm + sent), n * sizeof(int16_t));
        sent += n;
    }
    s_ringing = false;
    ESP_LOGI(TAG, "ring done (%u ms)", (unsigned)(s_ring_samples * 1000 / XIAOZHI_DOORBELL_SAMPLE_RATE));
#else
    ESP_LOGI(TAG, "ring disabled by XIAOZHI_DOORBELL_RING_ENABLE");
#endif
}

/* -------------------------------------------------------------------------- */
/* 事件编号与时间戳                                                            */
/* -------------------------------------------------------------------------- */

/* 板子无 RTC；SNTP 未接入前用开机时长命名，避免出现 1970 的假时间 */
static bool xiaozhi_doorbell_time_synced(struct tm *out)
{
    time_t now = time(NULL);
    if (now < 1700000000) { /* 2023-11-14 之前的都视为未授时 */
        return false;
    }
    return localtime_r(&now, out) != NULL;
}

static void xiaozhi_doorbell_make_event_id(char *out, size_t out_len)
{
    struct tm tm_now;
    if (xiaozhi_doorbell_time_synced(&tm_now)) {
        strftime(out, out_len, "EVT_%Y%m%d_%H%M%S", &tm_now);
    } else {
        snprintf(out, out_len, "EVT_BOOT_%lu", (unsigned long)(esp_timer_get_time() / 1000000));
    }
}

/* -------------------------------------------------------------------------- */
/* 录像钩子（M2 摄像头 + M5 MJPEG-AVI 录像模块注册进来才有实际动作）            */
/* -------------------------------------------------------------------------- */

static void xiaozhi_doorbell_record_stop(void);

static void xiaozhi_doorbell_record_start(const char *event_id)
{
    if (s_recorder.start == NULL) {
        ESP_LOGW(TAG, "recorder not registered (camera/recorder not ready), skip record");
        return;
    }
    if (s_recording) {
        /* 上一次还没结束：先停再开，保证一个事件一个文件 */
        xiaozhi_doorbell_record_stop();
    }

    esp_err_t err = s_recorder.start(event_id, XIAOZHI_DOORBELL_RECORD_MS, s_recorder.ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recorder start failed: %s", esp_err_to_name(err));
        return;
    }

    s_recording = true;
    s_record_deadline_us = esp_timer_get_time() + (int64_t)XIAOZHI_DOORBELL_RECORD_MS * 1000;
    ESP_LOGI(TAG, "record start: id=%s duration=%d ms", event_id, XIAOZHI_DOORBELL_RECORD_MS);
}

static void xiaozhi_doorbell_record_stop(void)
{
    if (!s_recording) {
        return;
    }
    s_recording = false;
    s_record_deadline_us = 0;
    if (s_recorder.stop != NULL) {
        s_recorder.stop(s_recorder.ctx);
    }
    ESP_LOGI(TAG, "record stop");
}

/* -------------------------------------------------------------------------- */
/* 事件日志（SD /LOG/event.csv，追加写；SD 未挂载则跳过）                       */
/* -------------------------------------------------------------------------- */

static void xiaozhi_doorbell_log_csv(const char *event_id, bool repeat, bool recording)
{
#if XIAOZHI_DOORBELL_LOG_ENABLE
    if (!xiaozhi_sdcard_ready()) {
        ESP_LOGW(TAG, "SD not ready, skip event log");
        return;
    }

    struct tm tm_now;
    char ts[32];
    if (xiaozhi_doorbell_time_synced(&tm_now)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
    } else {
        snprintf(ts, sizeof(ts), "boot_%lu_ms", (unsigned long)(esp_timer_get_time() / 1000));
    }

    bool need_header = (access(XIAOZHI_DOORBELL_LOG_PATH, F_OK) != 0);
    FILE *fp = fopen(XIAOZHI_DOORBELL_LOG_PATH, "a");
    if (fp == NULL) {
        ESP_LOGW(TAG, "open %s failed", XIAOZHI_DOORBELL_LOG_PATH);
        return;
    }
    if (need_header) {
        fprintf(fp, "time,event,id,repeat,record\n");
    }
    fprintf(fp, "%s,doorbell,%s,%d,%s\n", ts, event_id, repeat ? 1 : 0, recording ? "started" : "skipped");
    fclose(fp); /* 事件日志量很小，每次关闭即 flush，掉电不丢行 */
#else
    (void)event_id;
    (void)repeat;
    (void)recording;
#endif
}

/* -------------------------------------------------------------------------- */
/* 事件处理（门铃任务上下文，串行执行）                                         */
/* -------------------------------------------------------------------------- */

static void xiaozhi_doorbell_handle(xiaozhi_mode_src_t src, bool repeat)
{
    int64_t now_us = esp_timer_get_time();
    if (s_last_event_us != 0 &&
        (now_us - s_last_event_us) < (int64_t)XIAOZHI_DOORBELL_DEBOUNCE_MS * 1000) {
        ESP_LOGI(TAG, "trigger ignored (debounce %d ms)", XIAOZHI_DOORBELL_DEBOUNCE_MS);
        return;
    }
    s_last_event_us = now_us;
    s_event_count++;

    char event_id[sizeof(s_last_event_id)];
    xiaozhi_doorbell_make_event_id(event_id, sizeof(event_id));
    strncpy(s_last_event_id, event_id, sizeof(s_last_event_id) - 1);
    s_last_event_id[sizeof(s_last_event_id) - 1] = '\0';

    ESP_LOGI(TAG, "trigger #%u src=%s repeat=%d id=%s", (unsigned)s_event_count,
             xiaozhi_appmode_name(xiaozhi_appmode_get()), repeat ? 1 : 0, event_id);

    /* 1. 屏幕提示（标题由 appmode 维护，这里只改文本与表情） */
    xiaozhi_lvgl_set_text((char *)(repeat ? XIAOZHI_DOORBELL_TEXT_REPEAT : XIAOZHI_DOORBELL_TEXT_FIRST));
    xiaozhi_lvgl_set_emoji((char *)XIAOZHI_DOORBELL_EMOJI);

    /* 2. 先起录像（有后端时才真正开始），再响铃，避免漏掉门口第一时间 */
    xiaozhi_doorbell_record_start(event_id);

    /* 3. 提示音 */
    xiaozhi_doorbell_ring();

    /* 4. 云端通知（ws 未连接自动跳过） */
#if XIAOZHI_DOORBELL_WS_NOTIFY
    xiaozhi_ws_send_doorbell_event(event_id, s_recording);
#endif

    /* 5. 本地事件日志 */
    xiaozhi_doorbell_log_csv(event_id, repeat, s_recording);

    ESP_LOGI(TAG, "event handled: id=%s record=%s", event_id, s_recording ? "on" : "off");
}

/* -------------------------------------------------------------------------- */
/* 模式钩子                                                                     */
/* -------------------------------------------------------------------------- */

static void xiaozhi_doorbell_post(xiaozhi_mode_src_t src, bool repeat)
{
    if (s_queue == NULL) {
        ESP_LOGW(TAG, "queue not ready, drop trigger");
        return;
    }
    xiaozhi_doorbell_req_t req = {
        .src = src,
        .repeat = repeat,
    };
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, drop trigger");
    }
}

static void xiaozhi_doorbell_on_enter(xiaozhi_mode_t mode, xiaozhi_mode_src_t src, void *ctx)
{
    (void)mode;
    (void)ctx;
    /* 进入门铃模式即视为一次按铃 */
    xiaozhi_doorbell_post(src, false);
}

static void xiaozhi_doorbell_on_trigger(xiaozhi_mode_t mode, xiaozhi_mode_src_t src, void *ctx)
{
    (void)mode;
    (void)ctx;
    /* 已在门铃模式下再次长按 = 重复按铃 */
    xiaozhi_doorbell_post(src, true);
}

static void xiaozhi_doorbell_on_exit(xiaozhi_mode_t mode, void *ctx)
{
    (void)mode;
    (void)ctx;

    /* 1. 丢弃尚未处理的事件，避免退出模式后又响一次 */
    if (s_queue != NULL) {
        xiaozhi_doorbell_req_t drop;
        while (xQueueReceive(s_queue, &drop, 0) == pdTRUE) {
        }
    }

    /* 2. 立刻停录像（模式互斥：摄像头要交还给下一个模式） */
    xiaozhi_doorbell_record_stop();

    /* 3. 恢复默认文案，交给下一个模式/语音自行刷新 */
    xiaozhi_lvgl_set_text((char *)XIAOZHI_DOORBELL_TEXT_IDLE);
    xiaozhi_lvgl_set_emoji((char *)XIAOZHI_DOORBELL_EMOJI_IDLE);

    ESP_LOGI(TAG, "exit doorbell mode, total events=%u", (unsigned)s_event_count);
}

static void xiaozhi_doorbell_task(void *arg)
{
    (void)arg;
    xiaozhi_doorbell_req_t req;

    ESP_LOGI(TAG, "task started");

    while (1) {
        TickType_t wait = pdMS_TO_TICKS(XIAOZHI_DOORBELL_IDLE_MS);
        if (s_recording) {
            int64_t remain_ms = (s_record_deadline_us - esp_timer_get_time()) / 1000;
            TickType_t t = (remain_ms <= 0) ? 0 : pdMS_TO_TICKS((uint32_t)remain_ms + 1);
            if (t < wait) {
                wait = t;
            }
        }

        if (xQueueReceive(s_queue, &req, wait) == pdTRUE) {
            xiaozhi_doorbell_handle(req.src, req.repeat);
        }

        if (s_recording && esp_timer_get_time() >= s_record_deadline_us) {
            xiaozhi_doorbell_record_stop();
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 对外 API                                                                     */
/* -------------------------------------------------------------------------- */

void xiaozhi_doorbell_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    s_queue = xQueueCreate(XIAOZHI_DOORBELL_QUEUE_LEN, sizeof(xiaozhi_doorbell_req_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "create queue failed");
        return;
    }

    /* 栈放 PSRAM（xiaozhi_task_create 统一处理，失败自动回落内部 RAM）。
     * 本任务只做提示音播放与 SD 上的事件日志追加，PSRAM 栈安全。 */
    if (xiaozhi_task_create(xiaozhi_doorbell_task, "doorbell", XIAOZHI_STACK_4K, NULL, XIAOZHI_PRIO_NORMAL, NULL,
                            XIAOZHI_DOORBELL_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create task failed");
        return;
    }

    xiaozhi_mode_ops_t ops = {
        .on_enter = xiaozhi_doorbell_on_enter,
        .on_exit = xiaozhi_doorbell_on_exit,
        .on_trigger = xiaozhi_doorbell_on_trigger,
        .ctx = NULL,
    };
    esp_err_t err = xiaozhi_appmode_register(XIAOZHI_MODE_DOORBELL, &ops);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register mode hooks failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "init ok: ring=%d vad_gate=%d ws_notify=%d sd_log=%d record=%d ms", XIAOZHI_DOORBELL_RING_ENABLE,
             XIAOZHI_DOORBELL_RING_VAD_GATE, XIAOZHI_DOORBELL_WS_NOTIFY, XIAOZHI_DOORBELL_LOG_ENABLE,
             XIAOZHI_DOORBELL_RECORD_MS);
}

esp_err_t xiaozhi_doorbell_set_recorder(const xiaozhi_doorbell_recorder_t *recorder)
{
    if (recorder == NULL) {
        memset(&s_recorder, 0, sizeof(s_recorder));
        return ESP_OK;
    }
    if (recorder->start == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_recorder = *recorder;
    ESP_LOGI(TAG, "recorder registered");
    return ESP_OK;
}

bool xiaozhi_doorbell_is_ringing(void)
{
    return s_ringing;
}

bool xiaozhi_doorbell_is_recording(void)
{
    return s_recording;
}

uint32_t xiaozhi_doorbell_event_count(void)
{
    return s_event_count;
}

const char *xiaozhi_doorbell_last_event_id(void)
{
    return s_last_event_id;
}

#else /* XIAOZHI_DOORBELL_ENABLE == 0：空实现，不建任务、不注册钩子、不改界面 */

void xiaozhi_doorbell_init(void)
{
}

esp_err_t xiaozhi_doorbell_set_recorder(const xiaozhi_doorbell_recorder_t *recorder)
{
    (void)recorder;
    return ESP_ERR_NOT_SUPPORTED;
}

bool xiaozhi_doorbell_is_ringing(void)
{
    return false;
}

bool xiaozhi_doorbell_is_recording(void)
{
    return false;
}

uint32_t xiaozhi_doorbell_event_count(void)
{
    return 0;
}

const char *xiaozhi_doorbell_last_event_id(void)
{
    return "";
}

#endif /* XIAOZHI_DOORBELL_ENABLE */
