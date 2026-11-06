#include "xiaozhi_appmode.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "xiaozhi_common.h"
#include "xiaozhi_lvgl.h"
#include "xiaozhi_ws.h"

#define TAG "appmode"

#define XIAOZHI_APPMODE_TASK_CORE  0 /* 语音链路占满 core1，仲裁任务放 core0 */
#define XIAOZHI_APPMODE_QUEUE_LEN  8
#define XIAOZHI_APPMODE_SEND_TMO_MS 100

/* 每个模式可挂接的钩子数量：一个业务模块占一格（例如监控模式 = 预览 + 录像）。
 * 进入按注册顺序执行，退出按相反顺序执行，保证"后来的先释放"。 */
#define XIAOZHI_APPMODE_MAX_OPS    4

typedef struct {
    xiaozhi_mode_t mode;
    xiaozhi_mode_src_t src;
} xiaozhi_appmode_req_t;

/* 模式名（与 iot_desc.json / 语音口令一致） */
static const char *s_mode_names[XIAOZHI_MODE_MAX] = {"voice", "doorbell", "monitor"};
/* 屏幕标题（仅做状态提示，不影响语音界面其它对象） */
static const char *s_mode_titles[XIAOZHI_MODE_MAX] = {"AI小智｜语音", "AI小智｜门铃", "AI小智｜监控"};
/* 切换来源名（日志用） */
static const char *s_src_names[] = {"boot", "button", "voice", "remote", "timeout"};

static xiaozhi_mode_t s_mode = XIAOZHI_MODE_VOICE;
static xiaozhi_mode_ops_t s_ops[XIAOZHI_MODE_MAX][XIAOZHI_APPMODE_MAX_OPS];
static int s_ops_count[XIAOZHI_MODE_MAX];
static QueueHandle_t s_req_queue = NULL;
static bool s_inited = false;

/* -------------------------------------------------------------------------- */
/* 内部工具                                                                    */
/* -------------------------------------------------------------------------- */

static const char *xiaozhi_appmode_src_name(xiaozhi_mode_src_t src)
{
    if ((int)src >= 0 && (int)src < (int)(sizeof(s_src_names) / sizeof(s_src_names[0]))) {
        return s_src_names[src];
    }
    return "unknown";
}

static bool xiaozhi_appmode_name_to_mode(const char *name, xiaozhi_mode_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    /* 英文名（不区分大小写）与常见中文说法都接受 */
    if (strcasecmp(name, "voice") == 0 || strcasecmp(name, "chat") == 0 || strcmp(name, "语音") == 0) {
        *out = XIAOZHI_MODE_VOICE;
        return true;
    }
    if (strcasecmp(name, "doorbell") == 0 || strcasecmp(name, "door") == 0 || strcmp(name, "门铃") == 0) {
        *out = XIAOZHI_MODE_DOORBELL;
        return true;
    }
    if (strcasecmp(name, "monitor") == 0 || strcasecmp(name, "monitoring") == 0 || strcmp(name, "监控") == 0) {
        *out = XIAOZHI_MODE_MONITOR;
        return true;
    }
    return false;
}

/* 打印一个模式的状态（挂接钩子时会打印，便于确认模块已接入） */
static void xiaozhi_appmode_log_ops(void)
{
    for (int i = 0; i < XIAOZHI_MODE_MAX; i++) {
        for (int k = 0; k < s_ops_count[i]; k++) {
            const xiaozhi_mode_ops_t *op = &s_ops[i][k];
            ESP_LOGI(TAG, "handler registered: %s #%d (enter=%d exit=%d trigger=%d)", s_mode_names[i], k,
                     op->on_enter != NULL, op->on_exit != NULL, op->on_trigger != NULL);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 模式切换（只在 appmode 任务上下文中执行，天然串行）                          */
/* -------------------------------------------------------------------------- */

static void xiaozhi_appmode_apply(xiaozhi_mode_t mode, xiaozhi_mode_src_t src)
{
    if (mode < 0 || mode >= XIAOZHI_MODE_MAX) {
        return;
    }

    /* 已处于目标模式：只转发 trigger（例如门铃被再次按下） */
    if (mode == s_mode) {
        ESP_LOGI(TAG, "mode %s already active (src=%s)", s_mode_names[mode], xiaozhi_appmode_src_name(src));
        for (int k = 0; k < s_ops_count[mode]; k++) {
            if (s_ops[mode][k].on_trigger != NULL) {
                s_ops[mode][k].on_trigger(mode, src, s_ops[mode][k].ctx);
            }
        }
        return;
    }

    xiaozhi_mode_t old_mode = s_mode;

    /* 1. 先退出旧模式，释放摄像头/SD 等资源（倒序 = 后注册的先释放） */
    for (int k = s_ops_count[old_mode] - 1; k >= 0; k--) {
        if (s_ops[old_mode][k].on_exit != NULL) {
            s_ops[old_mode][k].on_exit(old_mode, s_ops[old_mode][k].ctx);
        }
    }

    /* 2. 切换状态 */
    s_mode = mode;

    /* 3. 再进入新模式，申请资源（正序 = 先预览后录像） */
    for (int k = 0; k < s_ops_count[mode]; k++) {
        if (s_ops[mode][k].on_enter != NULL) {
            s_ops[mode][k].on_enter(mode, src, s_ops[mode][k].ctx);
        }
    }

    ESP_LOGI(TAG, "mode: %s -> %s (src=%s)", s_mode_names[old_mode], s_mode_names[mode], xiaozhi_appmode_src_name(src));

    /* 4. 界面提示 + 向云端同步当前模式 */
    xiaozhi_appmode_refresh_ui();
    xiaozhi_ws_send_iot_mode_state(s_mode_names[mode]);
}

static void xiaozhi_appmode_task(void *arg)
{
    (void)arg;
    xiaozhi_appmode_req_t req;

    ESP_LOGI(TAG, "task started, default mode: %s", s_mode_names[s_mode]);
    /* 上电先刷新一次标题（此时 LVGL 可能尚未就绪，内部做了空判断） */
    xiaozhi_appmode_refresh_ui();

    while (1) {
        if (xQueueReceive(s_req_queue, &req, portMAX_DELAY) == pdTRUE) {
            xiaozhi_appmode_apply(req.mode, req.src);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 对外 API                                                                    */
/* -------------------------------------------------------------------------- */

void xiaozhi_appmode_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    memset(s_ops, 0, sizeof(s_ops));
    memset(s_ops_count, 0, sizeof(s_ops_count));
    s_mode = XIAOZHI_MODE_VOICE;

    s_req_queue = xQueueCreate(XIAOZHI_APPMODE_QUEUE_LEN, sizeof(xiaozhi_appmode_req_t));
    if (s_req_queue == NULL) {
        ESP_LOGE(TAG, "create queue failed");
        return;
    }

    BaseType_t ret = xiaozhi_task_create(xiaozhi_appmode_task, "appmode", XIAOZHI_STACK_4K, NULL, XIAOZHI_PRIO_HIGH,
                                         NULL, XIAOZHI_APPMODE_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create task failed");
        return;
    }

    ESP_LOGI(TAG, "init ok, modes: voice/doorbell/monitor (sw2/sw3 click, voice SetMode)");
}

esp_err_t xiaozhi_appmode_register(xiaozhi_mode_t mode, const xiaozhi_mode_ops_t *ops)
{
    if (mode < 0 || mode >= XIAOZHI_MODE_MAX || ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops_count[mode] >= XIAOZHI_APPMODE_MAX_OPS) {
        ESP_LOGE(TAG, "mode %s ops full (%d), register rejected", s_mode_names[mode], XIAOZHI_APPMODE_MAX_OPS);
        return ESP_ERR_NO_MEM;
    }
    s_ops[mode][s_ops_count[mode]] = *ops;
    s_ops_count[mode]++;
    xiaozhi_appmode_log_ops();
    return ESP_OK;
}

xiaozhi_mode_t xiaozhi_appmode_get(void)
{
    return s_mode;
}

const char *xiaozhi_appmode_name(xiaozhi_mode_t mode)
{
    if (mode < 0 || mode >= XIAOZHI_MODE_MAX) {
        return "unknown";
    }
    return s_mode_names[mode];
}

bool xiaozhi_appmode_is(xiaozhi_mode_t mode)
{
    return s_mode == mode;
}

esp_err_t xiaozhi_appmode_request(xiaozhi_mode_t mode, xiaozhi_mode_src_t src)
{
    if (mode < 0 || mode >= XIAOZHI_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_req_queue == NULL) {
        ESP_LOGW(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xiaozhi_appmode_req_t req = {
        .mode = mode,
        .src = src,
    };
    if (xQueueSend(s_req_queue, &req, pdMS_TO_TICKS(XIAOZHI_APPMODE_SEND_TMO_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "request queue full, drop %s", s_mode_names[mode]);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t xiaozhi_appmode_request_next(xiaozhi_mode_src_t src)
{
    xiaozhi_mode_t next = (xiaozhi_mode_t)((xiaozhi_appmode_get() + 1) % XIAOZHI_MODE_MAX);
    return xiaozhi_appmode_request(next, src);
}

void xiaozhi_appmode_button_switch(void)
{
    xiaozhi_appmode_request_next(XIAOZHI_MODE_SRC_BUTTON);
}

void xiaozhi_appmode_button_doorbell(void)
{
    xiaozhi_appmode_request(XIAOZHI_MODE_DOORBELL, XIAOZHI_MODE_SRC_BUTTON);
}

esp_err_t xiaozhi_appmode_voice_command(cJSON *parameters)
{
    xiaozhi_mode_t target = XIAOZHI_MODE_VOICE;
    xiaozhi_mode_t current = xiaozhi_appmode_get();

    /*
     * 兼容两种参数写法（云端/大模型可能给字符串也可能给数字）：
     *   {"mode":"doorbell"}   字符串：voice / doorbell / monitor（也接受 语音/门铃/监控）
     *   {"mode":2}            数字：1=voice 2=doorbell 3=monitor（0 也按 voice 处理）
     *   {"mode":"next"}       特殊：循环切到下一个模式
     */
    if (parameters != NULL) {
        cJSON *item = cJSON_GetObjectItem(parameters, "mode");
        if (item == NULL) {
            item = cJSON_GetObjectItem(parameters, "value");
        }

        if (cJSON_IsString(item)) {
            if (strcasecmp(item->valuestring, "next") == 0 || strcasecmp(item->valuestring, "switch") == 0) {
                ESP_LOGI(TAG, "voice: switch to next mode");
                return xiaozhi_appmode_request_next(XIAOZHI_MODE_SRC_VOICE);
            }
            if (!xiaozhi_appmode_name_to_mode(item->valuestring, &target)) {
                ESP_LOGW(TAG, "voice: unknown mode \"%s\"", item->valuestring);
                return ESP_ERR_INVALID_ARG;
            }
        } else if (cJSON_IsNumber(item)) {
            int v = item->valueint;
            if (v <= 1) {
                target = XIAOZHI_MODE_VOICE;
            } else if (v == 2) {
                target = XIAOZHI_MODE_DOORBELL;
            } else if (v == 3) {
                target = XIAOZHI_MODE_MONITOR;
            } else {
                ESP_LOGW(TAG, "voice: unknown mode number %d", v);
                return ESP_ERR_INVALID_ARG;
            }
        } else {
            ESP_LOGW(TAG, "voice: mode parameter type not supported");
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        /* 没带参数时按“循环切换”处理，避免语音口令失效 */
        ESP_LOGW(TAG, "voice: no parameters, switch to next mode");
        return xiaozhi_appmode_request_next(XIAOZHI_MODE_SRC_VOICE);
    }

    ESP_LOGI(TAG, "voice: mode %s -> %s", s_mode_names[current], s_mode_names[target]);
    return xiaozhi_appmode_request(target, XIAOZHI_MODE_SRC_VOICE);
}

/* -------------------------------------------------------------------------- */
/* 语音口令兜底：直接对 ASR 文本做关键词匹配                                    */
/*   云端有时只把口令当聊天回一句“好的”，不下发 Mode.SetMode，                  */
/*   这里用识别文本本地兜底，保证“切换到监控模式”这类说法一定能生效。           */
/* -------------------------------------------------------------------------- */

#if XIAOZHI_APPMODE_STT_FALLBACK

static bool xiaozhi_appmode_has_kw(const char *text, const char *kw)
{
    return (text != NULL && kw != NULL && kw[0] != '\0' && strstr(text, kw) != NULL);
}

bool xiaozhi_appmode_voice_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    /* 必须带“动作”词，避免聊天里顺口提到“监控/门铃”就误切模式 */
    bool has_action = xiaozhi_appmode_has_kw(text, "切换") || xiaozhi_appmode_has_kw(text, "切到") ||
                      xiaozhi_appmode_has_kw(text, "换到") || xiaozhi_appmode_has_kw(text, "进入") ||
                      xiaozhi_appmode_has_kw(text, "打开") || xiaozhi_appmode_has_kw(text, "开启") ||
                      xiaozhi_appmode_has_kw(text, "回到") || xiaozhi_appmode_has_kw(text, "返回") ||
                      xiaozhi_appmode_has_kw(text, "退出") || xiaozhi_appmode_has_kw(text, "关闭") ||
                      xiaozhi_appmode_has_kw(text, "模式") || xiaozhi_appmode_has_kw(text, "switch") ||
                      xiaozhi_appmode_has_kw(text, "mode");
    if (!has_action) {
        return false;
    }

    /* 1. 无明确目标：“切换模式 / 下一个模式” -> 循环切 */
    if (xiaozhi_appmode_has_kw(text, "下一个") || xiaozhi_appmode_has_kw(text, "下个模式") ||
        xiaozhi_appmode_has_kw(text, "next")) {
        ESP_LOGI(TAG, "voice-text: \"%s\" -> next", text);
        xiaozhi_appmode_request_next(XIAOZHI_MODE_SRC_VOICE);
        return true;
    }

    /* 2. 明确要回语音：“退出监控 / 关闭门铃 / 回到语音” */
    if (xiaozhi_appmode_has_kw(text, "退出监控") || xiaozhi_appmode_has_kw(text, "关闭监控") ||
        xiaozhi_appmode_has_kw(text, "退出门铃") || xiaozhi_appmode_has_kw(text, "关闭门铃") ||
        xiaozhi_appmode_has_kw(text, "回到语音") || xiaozhi_appmode_has_kw(text, "返回语音") ||
        xiaozhi_appmode_has_kw(text, "语音模式") || xiaozhi_appmode_has_kw(text, "聊天模式")) {
        ESP_LOGI(TAG, "voice-text: \"%s\" -> voice", text);
        xiaozhi_appmode_request(XIAOZHI_MODE_VOICE, XIAOZHI_MODE_SRC_VOICE);
        return true;
    }

    /* 3. 门铃 / 监控 */
    if (xiaozhi_appmode_has_kw(text, "门铃") || xiaozhi_appmode_has_kw(text, "doorbell")) {
        ESP_LOGI(TAG, "voice-text: \"%s\" -> doorbell", text);
        xiaozhi_appmode_request(XIAOZHI_MODE_DOORBELL, XIAOZHI_MODE_SRC_VOICE);
        return true;
    }
    if (xiaozhi_appmode_has_kw(text, "监控") || xiaozhi_appmode_has_kw(text, "monitor")) {
        ESP_LOGI(TAG, "voice-text: \"%s\" -> monitor", text);
        xiaozhi_appmode_request(XIAOZHI_MODE_MONITOR, XIAOZHI_MODE_SRC_VOICE);
        return true;
    }

    /* 4. 只说了“语音/voice” */
    if (xiaozhi_appmode_has_kw(text, "语音") || xiaozhi_appmode_has_kw(text, "voice")) {
        ESP_LOGI(TAG, "voice-text: \"%s\" -> voice", text);
        xiaozhi_appmode_request(XIAOZHI_MODE_VOICE, XIAOZHI_MODE_SRC_VOICE);
        return true;
    }

    return false;
}

#else /* XIAOZHI_APPMODE_STT_FALLBACK */

bool xiaozhi_appmode_voice_text(const char *text)
{
    (void)text;
    return false;
}

#endif /* XIAOZHI_APPMODE_STT_FALLBACK */

void xiaozhi_appmode_refresh_ui(void)
{
    xiaozhi_mode_t mode = xiaozhi_appmode_get();
    if (mode < 0 || mode >= XIAOZHI_MODE_MAX) {
        return;
    }
    /* xiaozhi_lvgl_set_title 内部对 title_label 做了空判断，LVGL 未就绪时直接返回 */
    xiaozhi_lvgl_set_title((char *)s_mode_titles[mode]);
}

bool xiaozhi_appmode_voice_enabled(void)
{
#if XIAOZHI_APPMODE_VOICE_GATE
    /* 门铃模式仍需要语音（响铃/对话），只有监控模式屏蔽语音链路 */
    return !xiaozhi_appmode_is(XIAOZHI_MODE_MONITOR);
#else
    return true;
#endif
}
