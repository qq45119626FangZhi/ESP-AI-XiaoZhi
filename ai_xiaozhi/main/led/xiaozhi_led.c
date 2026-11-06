#include "xiaozhi_led.h"

#include "xiaozhi_common.h"

#if XIAOZHI_LED_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "xiaozhi_camview.h"

#define TAG "led"

/* RMT 分辨率：10MHz（1 tick = 0.1us），时序用整数 tick 表示，避免浮点 */
#define XIAOZHI_LED_RES_HZ     (10 * 1000 * 1000)
/* WS2812B 时序（10MHz 分辨率）：0 码 0.4us 高 + 0.8us 低；1 码 0.8us 高 + 0.4us 低；
 * 单 bit 周期 1.2us（规格 1.25us ±0.6us，兼容） */
#define XIAOZHI_LED_T0H_TICKS  4
#define XIAOZHI_LED_T0L_TICKS  8
#define XIAOZHI_LED_T1H_TICKS  8
#define XIAOZHI_LED_T1L_TICKS  4
/* 帧间复位：低电平 > 50us 即锁存，取 350us 保守值 */
#define XIAOZHI_LED_RESET_US   350

/* 每个灯珠 24 bit，额外 1 个 symbol 余量 */
#define XIAOZHI_LED_SYMBOLS    (XIAOZHI_LED_COUNT * 24 + 1)

/* 栈与优先级统一见 xiaozhi_common.h 的 xiaozhi_task_stack_t / xiaozhi_task_prio_t */
#define XIAOZHI_LED_TASK_CORE  0 /* core1 留给语音链路 */

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static SemaphoreHandle_t s_lock = NULL;
static bool s_inited = false;

/* 当前颜色缓存：0x00RRGGBB */
static uint32_t s_pix[XIAOZHI_LED_COUNT];
static rmt_symbol_word_t s_symbols[XIAOZHI_LED_SYMBOLS];

/* -------------------------------------------------------------------------- */
/* 内部实现                                                                    */
/* -------------------------------------------------------------------------- */

static void xiaozhi_led_fill_symbol(rmt_symbol_word_t *sym, bool one)
{
    sym->level0 = 1;
    sym->duration0 = one ? XIAOZHI_LED_T1H_TICKS : XIAOZHI_LED_T0H_TICKS;
    sym->level1 = 0;
    sym->duration1 = one ? XIAOZHI_LED_T1L_TICKS : XIAOZHI_LED_T0L_TICKS;
}

/* 把 s_pix 整帧发出去（调用者需已持锁） */
static esp_err_t xiaozhi_led_flush(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    int n = 0;
    for (int i = 0; i < XIAOZHI_LED_COUNT; i++) {
        /* WS2812 数据顺序为 G-R-B，每字节高位先出 */
        uint8_t grb[3] = {
            (uint8_t)((s_pix[i] >> 8) & 0xFF),
            (uint8_t)((s_pix[i] >> 16) & 0xFF),
            (uint8_t)(s_pix[i] & 0xFF),
        };
        for (int k = 0; k < 3; k++) {
            for (int bit = 7; bit >= 0; bit--) {
                xiaozhi_led_fill_symbol(&s_symbols[n++], (grb[k] >> bit) & 0x01);
            }
        }
    }

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_chan, s_copy_encoder, s_symbols, (size_t)n * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "transmit failed: %s", esp_err_to_name(err));
        return err;
    }
    err = rmt_tx_wait_all_done(s_chan, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wait done failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 保持低电平超过复位时间，灯珠才锁存本帧数据 */
    esp_rom_delay_us(XIAOZHI_LED_RESET_US);
    return ESP_OK;
}

#if XIAOZHI_LED_FOLLOW_CAMVIEW
/*
 * 只读地跟随摄像头画面状态：
 *   xiaozhi_camview_is_open() == true 表示监控模式已成功启动摄像头并出图
 *   （camview 内部在预览任务创建成功后才置 s_img，失败会回滚为 NULL）。
 * 不在 camview/appmode 里加任何调用，避免影响已验收的监控链路。
 */
static void xiaozhi_led_follow_task(void *arg)
{
    (void)arg;
    bool last = false;

    /* 上电先灭一次灯（语音模式默认无画面） */
    xiaozhi_led_set_camera(false);

    while (1) {
        bool on = xiaozhi_camview_is_open();
        if (on != last) {
            last = on;
            xiaozhi_led_set_camera(on);
            ESP_LOGI(TAG, "camera view %s -> LED%u %s", on ? "open" : "closed",
                     (unsigned)(XIAOZHI_LED_CAMERA_INDEX + 2), on ? "green" : "off");
        }
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_LED_POLL_MS));
    }
}
#endif /* XIAOZHI_LED_FOLLOW_CAMVIEW */

/* -------------------------------------------------------------------------- */
/* 对外 API                                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t xiaozhi_led_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "create mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = XIAOZHI_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = XIAOZHI_LED_RES_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_copy_encoder_config_t copy_cfg = {0};
    err = rmt_new_copy_encoder(&copy_cfg, &s_copy_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        (void)rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        (void)rmt_del_encoder(s_copy_encoder);
        s_copy_encoder = NULL;
        (void)rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    for (int i = 0; i < XIAOZHI_LED_COUNT; i++) {
        s_pix[i] = 0;
    }
    s_inited = true;

    /* 上电全灭，避免残留旧颜色 */
    (void)xiaozhi_led_set_all(0, 0, 0);

    ESP_LOGI(TAG, "init ok: %d x WS2812 on GPIO%d (10MHz RMT), camera LED = LED%u",
             XIAOZHI_LED_COUNT, XIAOZHI_LED_GPIO, (unsigned)(XIAOZHI_LED_CAMERA_INDEX + 2));

#if XIAOZHI_LED_FOLLOW_CAMVIEW
    /* 栈放 PSRAM（xiaozhi_task_create 统一处理，失败自动回落内部 RAM） */
    if (xiaozhi_task_create(xiaozhi_led_follow_task, "led_follow", XIAOZHI_STACK_2K5, NULL, XIAOZHI_PRIO_LOW, NULL,
                            XIAOZHI_LED_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create follow task failed (LED stays off)");
    }
#endif
    return ESP_OK;
}

esp_err_t xiaozhi_led_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= XIAOZHI_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "take lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (s_pix[index] != v) {
        s_pix[index] = v;
        err = xiaozhi_led_flush();
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t xiaozhi_led_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "take lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    bool changed = false;
    for (int i = 0; i < XIAOZHI_LED_COUNT; i++) {
        if (s_pix[i] != v) {
            s_pix[i] = v;
            changed = true;
        }
    }

    esp_err_t err = changed ? xiaozhi_led_flush() : ESP_OK;

    xSemaphoreGive(s_lock);
    return err;
}

void xiaozhi_led_clear(void)
{
    (void)xiaozhi_led_set_all(0, 0, 0);
}

void xiaozhi_led_set_camera(bool on)
{
    if (on) {
        (void)xiaozhi_led_set(XIAOZHI_LED_CAMERA_INDEX, 0, XIAOZHI_LED_BRIGHT_G, 0);
    } else {
        (void)xiaozhi_led_set(XIAOZHI_LED_CAMERA_INDEX, 0, 0, 0);
    }
}

#else /* XIAOZHI_LED_ENABLE == 0：空实现 */

esp_err_t xiaozhi_led_init(void)
{
    return ESP_OK;
}

esp_err_t xiaozhi_led_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    (void)index;
    (void)r;
    (void)g;
    (void)b;
    return ESP_OK;
}

esp_err_t xiaozhi_led_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r;
    (void)g;
    (void)b;
    return ESP_OK;
}

void xiaozhi_led_clear(void)
{
}

void xiaozhi_led_set_camera(bool on)
{
    (void)on;
}

#endif /* XIAOZHI_LED_ENABLE */
