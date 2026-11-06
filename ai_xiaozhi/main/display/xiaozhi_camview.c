#include "xiaozhi_camview.h"

#include "xiaozhi_common.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_lvgl_port.h"
#include "jpeg_decoder.h"

#include "xiaozhi_appmode.h"
#include "xiaozhi_camera.h"
#include "xiaozhi_camstamp.h"

#define TAG "camview"

#if XIAOZHI_CAMVIEW_ENABLE && XIAOZHI_CAM_ENABLE

/* 预览任务：core0 / 优先级 3（低于语音链路的 5，避免抢占 AFE/opus）
 * 栈与优先级统一见 xiaozhi_common.h 的 xiaozhi_task_stack_t / xiaozhi_task_prio_t */
#define CAMVIEW_TASK_CORE    0
#define CAMVIEW_TASK_NAME    "cam_preview"

/* 关闭画面时等待预览任务退出的上限（grab 最长阻塞一帧，约 40ms） */
#define CAMVIEW_STOP_WAIT_MS 2000

static lv_obj_t *s_img = NULL;
static lv_image_dsc_t s_dsc;

static uint16_t *s_pix = NULL;    /* 给 LVGL 用的图像缓冲（PSRAM） */
static size_t s_pix_bytes = 0;
static uint16_t *s_dec = NULL;    /* JPEG 解码后的 RGB565 缓冲（PSRAM） */
static size_t s_dec_bytes = 0;

static int s_w = XIAOZHI_CAMVIEW_W; /* 当前画面尺寸（叠加 80x100 / 全屏 320x240） */
static int s_h = XIAOZHI_CAMVIEW_H;
static bool s_fullscreen = false;

static bool s_running = false;
static volatile bool s_visible = false;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_task_done = NULL;

/* 帧订阅（M5 录像 / M6 网页流）：默认全空，零开销 */
static portMUX_TYPE s_tap_mux = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_camview_frame_cb_t s_tap_cb[XIAOZHI_CAMVIEW_TAP_SLOTS];
static void *s_tap_ctx[XIAOZHI_CAMVIEW_TAP_SLOTS];

static int s_xmap[XIAOZHI_CAMVIEW_MAX_W];
static int s_xmap_src_w = -1;
static int s_xmap_dst_w = -1;

/* -------------------------------------------------------------------------- */
/* 缓冲管理                                                                     */
/* -------------------------------------------------------------------------- */

static void *camview_calloc_psram(size_t bytes)
{
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
    }
    return p;
}

static void camview_free_pix(void)
{
    if (s_pix != NULL) {
        heap_caps_free(s_pix);
        s_pix = NULL;
    }
    s_pix_bytes = 0;
    s_xmap_src_w = -1;
    s_xmap_dst_w = -1;
}

static void camview_free_dec(void)
{
    if (s_dec != NULL) {
        heap_caps_free(s_dec);
        s_dec = NULL;
    }
    s_dec_bytes = 0;
}

static esp_err_t camview_ensure_pix_buf(int w, int h)
{
    size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
    if (s_pix != NULL && s_pix_bytes >= bytes) {
        return ESP_OK;
    }
    camview_free_pix();
    s_pix = camview_calloc_psram(bytes);
    if (s_pix == NULL) {
        ESP_LOGE(TAG, "alloc image buffer %u bytes failed", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    s_pix_bytes = bytes;
    ESP_LOGI(TAG, "image buffer: %u bytes", (unsigned)bytes);
    return ESP_OK;
}

static esp_err_t camview_ensure_dec_buf(size_t bytes)
{
    if (s_dec != NULL && s_dec_bytes >= bytes) {
        return ESP_OK;
    }
    camview_free_dec();
    s_dec = camview_calloc_psram(bytes);
    if (s_dec == NULL) {
        ESP_LOGE(TAG, "alloc decode buffer %u bytes failed", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    s_dec_bytes = bytes;
    ESP_LOGI(TAG, "decode buffer: %u bytes", (unsigned)bytes);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* 缩放：最近邻（源 -> dst_w x dst_h），dst 由调用者按 dst_w*dst_h 分配          */
/* -------------------------------------------------------------------------- */

static void camview_scale(uint16_t *dst, const uint16_t *src, int src_w, int src_h, int dst_w, int dst_h,
                          bool swap_bytes)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || dst_w > XIAOZHI_CAMVIEW_MAX_W) {
        return;
    }

    /* 1:1 且无需换字节：直接整帧拷贝（全屏 QVGA 常用路径，省一次逐点查表） */
    if (!swap_bytes && src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_w * (size_t)dst_h * sizeof(uint16_t));
        return;
    }

    if (src_w != s_xmap_src_w || dst_w != s_xmap_dst_w) {
        for (int x = 0; x < dst_w; x++) {
            int sx = (int)(((int64_t)x * src_w) / dst_w);
            if (sx >= src_w) {
                sx = src_w - 1;
            }
            s_xmap[x] = sx;
        }
        s_xmap_src_w = src_w;
        s_xmap_dst_w = dst_w;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = (int)(((int64_t)y * src_h) / dst_h);
        if (sy >= src_h) {
            sy = src_h - 1;
        }
        const uint16_t *row = src + (size_t)sy * (size_t)src_w;
        uint16_t *out = dst + (size_t)y * dst_w;
        if (swap_bytes) {
            for (int x = 0; x < dst_w; x++) {
                uint16_t v = row[s_xmap[x]];
                out[x] = (uint16_t)((v >> 8) | (v << 8));
            }
        } else {
            for (int x = 0; x < dst_w; x++) {
                out[x] = row[s_xmap[x]];
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 帧订阅分发（在预览任务上下文同步执行，订阅者必须快速返回）                     */
/* -------------------------------------------------------------------------- */

static bool camview_has_tap(void)
{
    bool any = false;
    taskENTER_CRITICAL(&s_tap_mux);
    for (int i = 0; i < XIAOZHI_CAMVIEW_TAP_SLOTS; i++) {
        if (s_tap_cb[i] != NULL) {
            any = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_tap_mux);
    return any;
}

static void camview_dispatch_taps(const uint8_t *jpeg, size_t len, int w, int h)
{
    xiaozhi_camview_frame_cb_t cbs[XIAOZHI_CAMVIEW_TAP_SLOTS];
    void *ctxs[XIAOZHI_CAMVIEW_TAP_SLOTS];

    taskENTER_CRITICAL(&s_tap_mux);
    memcpy(cbs, s_tap_cb, sizeof(cbs));
    memcpy(ctxs, s_tap_ctx, sizeof(ctxs));
    taskEXIT_CRITICAL(&s_tap_mux);

    for (int i = 0; i < XIAOZHI_CAMVIEW_TAP_SLOTS; i++) {
        if (cbs[i] != NULL) {
            cbs[i](jpeg, len, w, h, ctxs[i]);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 预览任务                                                                     */
/* -------------------------------------------------------------------------- */
static void camview_task(void *arg)
{
    (void)arg;
    /* 几何参数在任务启动时固定下来，关闭过程中即使全局状态复位也不会错写缓冲 */
    const int dst_w = s_w;
    const int dst_h = s_h;
    const int fps = (s_fullscreen && XIAOZHI_CAMVIEW_FULL_FPS > 0) ? XIAOZHI_CAMVIEW_FULL_FPS
                     : (XIAOZHI_CAMVIEW_FPS > 0 ? XIAOZHI_CAMVIEW_FPS : 1);
    const TickType_t idle_ticks = pdMS_TO_TICKS(1000 / fps);
    uint32_t fail_count = 0;

    /* 每 5 s 打一行统计：区分"帧没来(grab)"、"帧来了但解不开(dec)"、"解开了但没画(show)"，
     * 出黑屏时看这一行就能定位卡在哪一环，不必再靠猜。 */
    int64_t stat_us = esp_timer_get_time();
    uint32_t stat_grab = 0, stat_show = 0, stat_dec = 0;

    ESP_LOGI(TAG, "preview task start: %dx%d @ %d fps%s", dst_w, dst_h, fps, s_fullscreen ? " (fullscreen)" : "");

    while (s_running) {
        camera_fb_t *fb = xiaozhi_camera_grab();
        if (fb == NULL) {
            fail_count++;
            stat_grab++;
            if (fail_count == 1 || (fail_count % 20) == 0) {
                ESP_LOGW(TAG, "grab failed (%u times)", (unsigned)fail_count);
            }
            /* 短等待，保证 close() 能尽快拿到任务退出信号 */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        fail_count = 0;

        /* M5/M6：把同一帧 JPEG 分发给订阅者（录像/网页流），不额外 grab 摄像头。
         * 没有订阅者时 camview_has_tap() 只是一次加锁查表，开销可忽略。 */
        if (fb->format == PIXFORMAT_JPEG && camview_has_tap()) {
            camview_dispatch_taps(fb->buf, fb->len, fb->width, fb->height);
        }

        const uint16_t *src = NULL;
        int src_w = 0;
        int src_h = 0;
        bool swap_bytes = false;

        if (fb->format == PIXFORMAT_JPEG) {
            size_t need = (size_t)fb->width * fb->height * sizeof(uint16_t);
            if (camview_ensure_dec_buf(need) == ESP_OK) {
                esp_jpeg_image_cfg_t cfg = {
                    .indata = fb->buf,
                    .indata_size = fb->len,
                    .outbuf = (uint8_t *)s_dec,
                    .outbuf_size = s_dec_bytes,
                    .out_format = JPEG_IMAGE_FORMAT_RGB565,
                    .out_scale = (XIAOZHI_CAMVIEW_JPEG_SCALE_DIV >= 8) ? JPEG_IMAGE_SCALE_1_8
                                 : (XIAOZHI_CAMVIEW_JPEG_SCALE_DIV >= 4) ? JPEG_IMAGE_SCALE_1_4
                                 : (XIAOZHI_CAMVIEW_JPEG_SCALE_DIV >= 2) ? JPEG_IMAGE_SCALE_1_2
                                                                        : JPEG_IMAGE_SCALE_0,
                    /* 必须给小端：LVGL 按本机字节序解释图像数据，大端由
                     * esp_lvgl_port 的 swap_bytes 在 flush 时统一转换 */
                    .flags = {.swap_color_bytes = XIAOZHI_CAMVIEW_JPEG_SWAP_BYTES},
                };
                esp_jpeg_image_output_t out = {0};
                esp_err_t err = esp_jpeg_decode(&cfg, &out);
                if (err == ESP_OK) {
                    src = s_dec;
                    src_w = out.width;
                    src_h = out.height;
                    swap_bytes = false; /* 解码已转成本机小端 */
                } else {
                    stat_dec++;
                    ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
                }
            }
        } else {
            src = (const uint16_t *)fb->buf;
            src_w = fb->width;
            src_h = fb->height;
            swap_bytes = XIAOZHI_CAMVIEW_SWAP_SRC_BYTES;
        }

        if (src != NULL && s_visible && s_img != NULL && s_pix != NULL) {
            /* 持 LVGL 锁写入图像缓冲，避免与渲染任务同时访问 */
            if (lvgl_port_lock(50)) {
                camview_scale(s_pix, src, src_w, src_h, dst_w, dst_h, swap_bytes);
                /* 北京时间水印：直接画进像素缓冲，跟随帧刷新，不额外触发 LVGL 重绘 */
                xiaozhi_camstamp_draw(s_pix, dst_w, dst_h);
                lv_obj_invalidate(s_img);
                lvgl_port_unlock();
                stat_show++;
            }
        }

        xiaozhi_camera_return(fb);

        /* 5 s 一次统计：show 为 0 且 grab 一直涨 = 摄像头没出帧（cam_hal 层面）；
         * grab 正常但 dec 在涨 = 解码失败；两者都正常但画面仍黑 = LVGL 侧问题。 */
        if ((esp_timer_get_time() - stat_us) >= 5000000) {
            ESP_LOGI(TAG, "stat: shown=%u grab_fail=%u dec_fail=%u", (unsigned)stat_show, (unsigned)stat_grab,
                     (unsigned)stat_dec);
            stat_us = esp_timer_get_time();
            stat_show = 0;
            stat_grab = 0;
            stat_dec = 0;
        }

        if (idle_ticks > 0) {
            vTaskDelay(idle_ticks);
        } else {
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "preview task stop");
    s_task = NULL;
    if (s_task_done != NULL) {
        xSemaphoreGive(s_task_done);
    }
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* 模式钩子：只有监控模式持有摄像头，退出即释放                                  */
/* -------------------------------------------------------------------------- */

#if XIAOZHI_CAMVIEW_MODE_BIND

static void camview_mode_monitor_enter(xiaozhi_mode_t mode, xiaozhi_mode_src_t src, void *ctx)
{
    (void)mode;
    (void)ctx;
    ESP_LOGI(TAG, "monitor enter (src=%d): start camera + fullscreen view", (int)src);

    if (!xiaozhi_camera_ready()) {
        esp_err_t err = xiaozhi_camera_init(XIAOZHI_CAM_FMT_JPEG);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
            return;
        }
    }

    esp_err_t err = xiaozhi_camview_open(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open fullscreen view failed: %s, camera will be released", esp_err_to_name(err));
        (void)xiaozhi_camview_close();
#if !XIAOZHI_CAM_KEEP_INIT
        (void)xiaozhi_camera_deinit();
#endif
    }
}

static void camview_mode_monitor_exit(xiaozhi_mode_t mode, void *ctx)
{
    (void)mode;
    (void)ctx;
    ESP_LOGI(TAG, "monitor exit: close view");
    (void)xiaozhi_camview_close();
#if !XIAOZHI_CAM_KEEP_INIT
    if (xiaozhi_camera_ready()) {
        (void)xiaozhi_camera_deinit();
    }
#else
    /* 本板 OV2640 无 PWDN/RESET 控制，deinit 后二次 init 会 SCCB 超时，
     * 因此保持摄像头初始化，只关闭预览视图（详见 xiaozhi_camera.h 的宏说明）。 */
    ESP_LOGI(TAG, "monitor exit: keep camera initialized (XIAOZHI_CAM_KEEP_INIT=1)");
#endif
}

#endif /* XIAOZHI_CAMVIEW_MODE_BIND */

/* -------------------------------------------------------------------------- */
/* 对外 API                                                                     */
/* -------------------------------------------------------------------------- */

esp_err_t xiaozhi_camview_init(void)
{
    if (s_task_done == NULL) {
        s_task_done = xSemaphoreCreateBinary();
        if (s_task_done == NULL) {
            ESP_LOGE(TAG, "create task-done semaphore failed");
            return ESP_ERR_NO_MEM;
        }
    }

#if XIAOZHI_CAMVIEW_MODE_BIND
    /* 默认路径：什么也不创建，只在监控模式进入时才启动摄像头与全屏预览 */
    xiaozhi_mode_ops_t ops = {
        .on_enter = camview_mode_monitor_enter,
        .on_exit = camview_mode_monitor_exit,
        .on_trigger = NULL,
        .ctx = NULL,
    };
    esp_err_t err = xiaozhi_appmode_register(XIAOZHI_MODE_MONITOR, &ops);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register monitor hooks failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "mode-bound: camera runs only in MONITOR mode (fullscreen %dx%d @ %d fps)",
             XIAOZHI_CAMVIEW_FULL_W, XIAOZHI_CAMVIEW_FULL_H, XIAOZHI_CAMVIEW_FULL_FPS);
    return ESP_OK;
#else
    /* 旧行为：上电即右上角常驻叠加预览（需要摄像头已初始化） */
    return xiaozhi_camview_open(false);
#endif
}

esp_err_t xiaozhi_camview_open(bool fullscreen)
{
    if (!xiaozhi_camera_ready()) {
        ESP_LOGW(TAG, "open skipped: camera not ready");
        return ESP_ERR_INVALID_STATE;
    }

    const int w = fullscreen ? XIAOZHI_CAMVIEW_FULL_W : XIAOZHI_CAMVIEW_W;
    const int h = fullscreen ? XIAOZHI_CAMVIEW_FULL_H : XIAOZHI_CAMVIEW_H;

    if (s_img != NULL) {
        if (w == s_w && h == s_h) {
            xiaozhi_camview_set_visible(true);
            s_running = true;
            /* 视图对象还在、但预览任务已经不存在（上次 close 等任务/等 LVGL 锁超时提前返回，
             * 或任务异常退出）时，必须补建任务；否则 is_open() 仍为 true、/stream 也能挂上，
             * 却永远没有帧（实机表现：已连上但一直黑屏 + cam_hal 报 EV-VSYNC-OVF）。 */
            if (s_task == NULL) {
                if (xiaozhi_task_create(camview_task, CAMVIEW_TASK_NAME, XIAOZHI_STACK_8K, NULL, XIAOZHI_PRIO_NORMAL,
                                        NULL, CAMVIEW_TASK_CORE) != pdPASS) {
                    ESP_LOGE(TAG, "reopen: recreate preview task failed");
                    s_running = false;
                    return ESP_ERR_NO_MEM;
                }
                ESP_LOGW(TAG, "reopen: preview task was missing, recreated");
            }
            return ESP_OK;
        }
        /* 尺寸/形态变化：整体重建，避免图像对象继续引用旧缓冲 */
        esp_err_t err = xiaozhi_camview_close();
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = camview_ensure_pix_buf(w, h);
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = w;
    s_dsc.header.h = h;
    s_dsc.header.stride = w * (int)sizeof(uint16_t);
    s_dsc.data_size = (uint32_t)((size_t)w * (size_t)h * sizeof(uint16_t));
    s_dsc.data = (const uint8_t *)s_pix;

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock failed");
        camview_free_pix();
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *screen = lv_screen_active();
    s_img = lv_image_create(screen);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_set_size(s_img, w, h);
    if (fullscreen) {
        /* 全屏：铺满整个屏幕，去掉边框，叠在最上层（标题/emoji/文本被画面覆盖） */
        lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_width(s_img, 0, 0);
    } else {
        lv_obj_align(s_img, LV_ALIGN_TOP_RIGHT, -XIAOZHI_CAMVIEW_OFFSET_X, XIAOZHI_CAMVIEW_OFFSET_Y);
        lv_obj_set_style_border_width(s_img, 1, 0);
        lv_obj_set_style_border_color(s_img, lv_color_hex(0x303030), 0);
    }
    lv_obj_set_scrollable(s_img, false);
    lv_obj_set_style_radius(s_img, 0, 0);
    lv_obj_set_style_pad_all(s_img, 0, 0);
    lv_obj_set_hidden(s_img, false);
    lv_obj_move_foreground(s_img);
    lvgl_port_unlock();

    s_w = w;
    s_h = h;
    s_fullscreen = fullscreen;
    s_xmap_src_w = -1;
    s_xmap_dst_w = -1;
    s_visible = true;
    s_running = true;

    if (s_task == NULL) {
        if (xiaozhi_task_create(camview_task, CAMVIEW_TASK_NAME, XIAOZHI_STACK_8K, NULL, XIAOZHI_PRIO_NORMAL, NULL,
                                CAMVIEW_TASK_CORE) != pdPASS) {
            ESP_LOGE(TAG, "create preview task failed");
            s_running = false;
            s_visible = false;
            (void)xiaozhi_camview_close();
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "view open: %dx%d (%s), psram free: %u KB", w, h, fullscreen ? "fullscreen" : "overlay",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

esp_err_t xiaozhi_camview_close(void)
{
    if (s_img == NULL && s_task == NULL) {
        s_visible = false;
        s_running = false;
        return ESP_OK;
    }

    s_visible = false;
    s_running = false;

    /* 1. 先等预览任务退出（它可能正持有一帧或正等 LVGL 锁），再动图像对象与缓冲 */
    if (s_task != NULL && s_task_done != NULL) {
        if (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(CAMVIEW_STOP_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "close: wait preview task timeout, keep image/buffers");
            return ESP_ERR_TIMEOUT;
        }
    }

    /* 2. 删除 LVGL 对象（此后 s_pix 不再被引用） */
    if (s_img != NULL) {
        if (!lvgl_port_lock(500)) {
            ESP_LOGE(TAG, "close: lvgl lock failed, keep image");
            return ESP_ERR_TIMEOUT;
        }
        lv_obj_delete(s_img);
        s_img = NULL;
        lvgl_port_unlock();
    }

    /* 3. 归还 PSRAM */
    camview_free_pix();
    camview_free_dec();

    s_w = XIAOZHI_CAMVIEW_W;
    s_h = XIAOZHI_CAMVIEW_H;
    s_fullscreen = false;

    ESP_LOGI(TAG, "view closed, psram free: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return ESP_OK;
}

bool xiaozhi_camview_is_open(void)
{
    return s_img != NULL;
}

void xiaozhi_camview_set_visible(bool visible)
{
    if (s_img == NULL) {
        s_visible = visible;
        return;
    }
    s_visible = visible;
    if (lvgl_port_lock(100)) {
        if (visible) {
            lv_obj_set_hidden(s_img, false);
            lv_obj_move_foreground(s_img);
            lv_obj_invalidate(s_img);
        } else {
            lv_obj_set_hidden(s_img, true);
        }
        lvgl_port_unlock();
    }
}

bool xiaozhi_camview_is_visible(void)
{
    return s_visible;
}

void xiaozhi_camview_stop(void)
{
    (void)xiaozhi_camview_close();
}

esp_err_t xiaozhi_camview_set_frame_tap(int slot, xiaozhi_camview_frame_cb_t cb, void *ctx)
{
    if (slot < 0 || slot >= XIAOZHI_CAMVIEW_TAP_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_tap_mux);
    s_tap_cb[slot] = cb;
    s_tap_ctx[slot] = ctx;
    taskEXIT_CRITICAL(&s_tap_mux);
    ESP_LOGI(TAG, "frame tap %d %s", slot, (cb != NULL) ? "attached" : "detached");
    return ESP_OK;
}

#else /* XIAOZHI_CAMVIEW_ENABLE == 0（或摄像头关闭）：空实现 */

esp_err_t xiaozhi_camview_init(void)
{
    return ESP_OK;
}
esp_err_t xiaozhi_camview_open(bool fullscreen)
{
    (void)fullscreen;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_camview_close(void)
{
    return ESP_OK;
}
bool xiaozhi_camview_is_open(void)
{
    return false;
}
void xiaozhi_camview_set_visible(bool visible)
{
    (void)visible;
}
bool xiaozhi_camview_is_visible(void)
{
    return false;
}
void xiaozhi_camview_stop(void)
{
}
esp_err_t xiaozhi_camview_set_frame_tap(int slot, xiaozhi_camview_frame_cb_t cb, void *ctx)
{
    (void)slot;
    (void)cb;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
