#include "xiaozhi_time.h"

#include "xiaozhi_common.h"

#if XIAOZHI_TIME_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "xiaozhi_lvgl.h"
#include "xiaozhi_camview.h"

#define TAG "time"

/* 已授时的最小 epoch（2023-11-14）：小于它说明 RTC 还是 1970 初值 */
#define XIAOZHI_TIME_EPOCH_VALID   1700000000

static TaskHandle_t s_task = NULL;
static volatile bool s_synced = false;

/* 设置时区（必须在校时成功前设置，localtime_r 才按东八区换算） */
static void time_apply_tz(void)
{
    setenv("TZ", XIAOZHI_TIME_TZ, 1);
    tzset();
}

bool xiaozhi_time_format(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return false;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    if (now < XIAOZHI_TIME_EPOCH_VALID || localtime_r(&now, &tm_now) == NULL) {
        buf[0] = '\0';
        return false;
    }
    strftime(buf, len, XIAOZHI_TIME_FMT, &tm_now);
    return true;
}

bool xiaozhi_time_is_synced(void)
{
    return s_synced;
}

static void time_task(void *arg)
{
    (void)arg;
    char buf[32];

    /* 1. 校时（只做一次；失败最多重试 XIAOZHI_TIME_SYNC_RETRY 次） */
    const esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(XIAOZHI_TIME_NTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sntp start: server=%s tz=%s", XIAOZHI_TIME_NTP_SERVER, XIAOZHI_TIME_TZ);
        for (int attempt = 1; attempt <= XIAOZHI_TIME_SYNC_RETRY; attempt++) {
            err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(XIAOZHI_TIME_SYNC_TIMEOUT_MS));
            if (err == ESP_OK) {
                s_synced = true;
                break;
            }
            ESP_LOGW(TAG, "sntp sync failed (%d/%d): %s", attempt, XIAOZHI_TIME_SYNC_RETRY, esp_err_to_name(err));
        }
        /* 校验一次即可：成功/失败都停止 SNTP，避免后台周期请求 */
        esp_netif_sntp_deinit();
    }

    if (s_synced && xiaozhi_time_format(buf, sizeof(buf))) {
        ESP_LOGI(TAG, "synced: %s (epoch=%lld)", buf, (long long)time(NULL));
    } else {
        ESP_LOGW(TAG, "time not synced, screen keeps \"--\"");
    }

    /* 2. 每秒刷新一次屏幕时间（未授时不刷，保持 "--"）
     *    监控模式全屏预览时标题栏被画面整屏覆盖，此时不刷新：既省一次 LVGL 重绘，
     *    也避免与预览的 SPI 推送抢同一个 panel io（历史上曾触发 tx_color 入队失败）。 */
    while (1) {
        if (s_synced && !xiaozhi_camview_is_open() && xiaozhi_time_format(buf, sizeof(buf))) {
            xiaozhi_lvgl_set_datetime(buf);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t xiaozhi_time_start(void)
{
    /* 只启动一次（WiFi 重连/GOT_IP 多次触发也只校一次） */
    if (s_task != NULL) {
        return ESP_OK;
    }
    time_apply_tz();

    /* 栈放 PSRAM（xiaozhi_task_create 统一处理，失败自动回落内部 RAM） */
    if (xiaozhi_task_create(time_task, "time", XIAOZHI_STACK_4K, NULL, XIAOZHI_PRIO_LOW, &s_task,
                            XIAOZHI_TIME_TASK_CORE) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "create time task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* XIAOZHI_TIME_ENABLE */

/* 关闭时保持接口可用（空实现），调用方不需要加宏判断 */
esp_err_t xiaozhi_time_start(void)
{
    return ESP_OK;
}

bool xiaozhi_time_is_synced(void)
{
    return false;
}

bool xiaozhi_time_format(char *buf, size_t len)
{
    if (buf != NULL && len > 0) {
        buf[0] = '\0';
    }
    return false;
}

#endif /* XIAOZHI_TIME_ENABLE */
