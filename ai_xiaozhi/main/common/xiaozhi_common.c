#include "xiaozhi_common.h"

#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define TAG "common"

/* 栈优先 PSRAM：内部 RAM 只留给 DMA（mbedtls AES-DMA / 摄像头 / LVGL / I2S）。
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y 已开，PSRAM 上跑任务栈是官方支持用法。
 * PSRAM 分配失败（极少见）时回落内部 RAM，保证功能不因内存策略而丢。 */
BaseType_t xiaozhi_task_create(TaskFunction_t task_func, const char *name, xiaozhi_task_stack_t stack,
                               void *arg, xiaozhi_task_prio_t prio, TaskHandle_t *handle, int core_id)
{
    const char *task_name = (name != NULL) ? name : "?";
    bool in_psram = true;

    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(task_func, name, (uint32_t)stack, arg, (UBaseType_t)prio, handle,
                                                     core_id, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "%s: psram stack failed, fallback to internal RAM", task_name);
        in_psram = false;
        ret = xTaskCreatePinnedToCore(task_func, name, (uint32_t)stack, arg, (UBaseType_t)prio, handle, core_id);
    }

    if (ret == pdPASS) {
        /* 每个任务一行，启动日志里能一眼确认栈到底落在哪：现场排查内部 RAM 不足时很关键 */
        ESP_LOGI(TAG, "%s: stack=%u B prio=%d core=%d -> %s", task_name, (unsigned)stack, (int)prio, core_id,
                 in_psram ? "PSRAM" : "internal RAM");
    } else {
        ESP_LOGE(TAG, "%s: create task failed (stack=%u B)", task_name, (unsigned)stack);
    }
    return ret;
}

/* 环形缓冲区同样放 PSRAM：音频链路三个 ringbuf 各 16KB，放内部 RAM 会直接吃掉 48KB。 */
RingbufHandle_t xiaozhi_ringbuf_create(xiaozhi_ringbuf_size_t size, RingbufferType_t type)
{
    RingbufHandle_t handle = xRingbufferCreateWithCaps((size_t)size, type, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (handle == NULL) {
        ESP_LOGW(TAG, "ringbuf %u B: psram failed, fallback to internal RAM", (unsigned)size);
        handle = xRingbufferCreate((size_t)size, type);
    }
    if (handle == NULL) {
        ESP_LOGE(TAG, "ringbuf %u B: create failed", (unsigned)size);
    }
    return handle;
}
