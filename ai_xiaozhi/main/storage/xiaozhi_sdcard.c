#include "xiaozhi_sdcard.h"

#include "xiaozhi_common.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define TAG "sdcard"

/* SDMMC 1-bit 引脚（来自原理图：CARD1 的 CLK/CMD/D0，DAT1~3 与 CD 全 NC） */
#define XIAOZHI_SD_PIN_CLK 14
#define XIAOZHI_SD_PIN_CMD 13
#define XIAOZHI_SD_PIN_D0  15

/* 1-bit 模式下的时钟：默认 20MHz（文档速率基线），需要提速改成 SDMMC_FREQ_HIGHSPEED */
#define XIAOZHI_SD_FREQ_KHZ SDMMC_FREQ_DEFAULT

/* 挂载失败自动降频重试：1 = 最高档失败后退半档，再失败退到 XIAOZHI_SD_FREQ_MIN_KHZ，
 * 成功后保持该档（下次拔卡重插会从最高档重新试）。
 * 现场依据：日志里出现
 *   W (...) vfs_fat_sdmmc: failed to mount card (1)
 *   E (...) vfs_fat_sdmmc: mount_to_vfs failed (0xffffffff).
 * FRESULT=1 即 FR_DISK_ERR —— 说明 400kHz 识别阶段是通的（sdmmc_card_init 已成功，
 * 卡在位），但升到 20MHz 读引导扇区出错，多为卡座接触/走线/供电造成的信号完整性
 * 余量不足；降频通常立刻稳定。0 = 只用 XIAOZHI_SD_FREQ_KHZ 一档（旧行为）。 */
#define XIAOZHI_SD_FREQ_FALLBACK 1
#define XIAOZHI_SD_FREQ_MIN_KHZ  5000
#define XIAOZHI_SD_FREQ_STEPS    3

#define XIAOZHI_SD_MAX_FILES 8
#define XIAOZHI_SD_ALLOC_UNIT (16 * 1024)

/* 栈与优先级统一见 xiaozhi_common.h 的 xiaozhi_task_stack_t / xiaozhi_task_prio_t */
#define XIAOZHI_SD_TASK_CORE  0 /* 语音链路占满 core1，守护任务放 core0 */

#define XIAOZHI_SD_SELFTEST_BYTES (512 * 1024)
#define XIAOZHI_SD_SELFTEST_CHUNK 4096

/* DCIM 目录（循环覆盖只在这个目录内删文件，绝不碰 LOG/CONFIG） */
#define XIAOZHI_SD_DCIM_DIR XIAOZHI_SD_MOUNT_POINT "/DCIM"

/* 目录/文件名缓冲 */
#define XIAOZHI_SD_PATH_MAX 320

#if XIAOZHI_SD_ENABLE

static SemaphoreHandle_t s_lock = NULL;
static sdmmc_card_t *s_card = NULL;
static volatile bool s_mounted = false;
static volatile bool s_inited = false;
static volatile bool s_auto_recover = true;
static int s_fail_count = 0;
/* 当前尝试的时钟档位：0 = 最高档 XIAOZHI_SD_FREQ_KHZ，逐档减半（见 xiaozhi_sdcard_freq_khz） */
static int s_freq_step = 0;

/* -------------------------------------------------------------------------- */
/* 内部工具                                                                    */
/* -------------------------------------------------------------------------- */

/* 读容量；返回 false 表示当前不可用（例如卡被拔掉） */
static bool xiaozhi_sdcard_stat(uint64_t *total, uint64_t *free_bytes)
{
    uint64_t t = 0;
    uint64_t f = 0;
    if (!s_mounted) {
        return false;
    }
    if (esp_vfs_fat_info(XIAOZHI_SD_MOUNT_POINT, &t, &f) != ESP_OK) {
        return false;
    }
    if (total != NULL) {
        *total = t;
    }
    if (free_bytes != NULL) {
        *free_bytes = f;
    }
    return true;
}

/* 容量缓存：只在挂载流程与 5 s 守护任务里刷新。
 * 这样 /api/status、首页等高频调用方直接读缓存，不会在 httpd 任务里做阻塞的
 * FATFS I/O —— 否则卡异常（写失败/接触不良）时 esp_vfs_fat_info() 会卡住，
 * 把整个网页服务（含 /api/mode 切模式）一起拖死。 */
static volatile uint64_t s_cached_total;
static volatile uint64_t s_cached_free;
static volatile bool s_cached_ok;

static void xiaozhi_sdcard_cache_update(void)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (xiaozhi_sdcard_stat(&total, &free_bytes)) {
        s_cached_total = total;
        s_cached_free = free_bytes;
        s_cached_ok = true;
    } else {
        s_cached_ok = false;
    }
}

static void xiaozhi_sdcard_cache_invalidate(void)
{
    s_cached_ok = false;
    s_cached_total = 0;
    s_cached_free = 0;
}

/* 建目录（单层，父目录必须已存在）；已存在且是目录视为成功 */
static esp_err_t xiaozhi_sdcard_mkdir_if_absent(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (mkdir(path, 0777) == 0) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", path, errno);
    return ESP_FAIL;
}

/* 建好基础目录：DCIM / LOG / CONFIG */
static void xiaozhi_sdcard_prepare_dirs(void)
{
    xiaozhi_sdcard_mkdir_if_absent(XIAOZHI_SD_DCIM_DIR);
    xiaozhi_sdcard_mkdir_if_absent(XIAOZHI_SD_MOUNT_POINT "/LOG");
    xiaozhi_sdcard_mkdir_if_absent(XIAOZHI_SD_MOUNT_POINT "/CONFIG");
}

/* 递归找“文件名最小”的普通文件（文件名按时间命名，字典序即时间序）。
 * 只在 DCIM 下最多递归 2 层（/DCIM/<日期>/<文件>）。*/
static bool xiaozhi_sdcard_find_oldest(const char *dir, char *out, size_t out_len, int depth)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return false;
    }

    bool found = false;
    struct dirent *entry = NULL;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[XIAOZHI_SD_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (n <= 0 || n >= (int)sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (depth < 2 && xiaozhi_sdcard_find_oldest(path, out, out_len, depth + 1)) {
                found = true;
            }
            continue;
        }

        if (!found || strcmp(path, out) < 0) {
            strncpy(out, path, out_len - 1);
            out[out_len - 1] = '\0';
            found = true;
        }
    }

    closedir(d);
    return found;
}

/* 第 step 档时钟：0 = 最高档，逐档减半，但不低于 XIAOZHI_SD_FREQ_MIN_KHZ */
static int xiaozhi_sdcard_freq_khz(int step)
{
#if XIAOZHI_SD_FREQ_FALLBACK
    int freq = XIAOZHI_SD_FREQ_KHZ;
    for (int i = 0; (i < step) && (i < XIAOZHI_SD_FREQ_STEPS); i++) {
        freq /= 2;
    }
    return (freq < XIAOZHI_SD_FREQ_MIN_KHZ) ? XIAOZHI_SD_FREQ_MIN_KHZ : freq;
#else
    (void)step;
    return XIAOZHI_SD_FREQ_KHZ;
#endif
}

/* 还有更低一档可退吗（关闭降频时永远返回 false） */
static bool xiaozhi_sdcard_freq_can_step_down(void)
{
    return xiaozhi_sdcard_freq_khz(s_freq_step + 1) < xiaozhi_sdcard_freq_khz(s_freq_step);
}

/* 挂载/卸载串行化（业务任务与守护任务可能同时调用） */
static inline void xiaozhi_sdcard_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void xiaozhi_sdcard_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* -------------------------------------------------------------------------- */
/* 挂载 / 卸载                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t xiaozhi_sdcard_mount(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    xiaozhi_sdcard_lock();
    if (s_mounted) {
        xiaozhi_sdcard_unlock();
        return ESP_OK;
    }

    /* 1-bit 总线：只保留 1BIT 能力，DEINIT_ARG 让卸载时按 slot 释放 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.slot = SDMMC_HOST_SLOT_1;
    const int freq_khz = xiaozhi_sdcard_freq_khz(s_freq_step);
    host.max_freq_khz = freq_khz;

    /* 引脚必须显式指定：S3 的 slot 默认值用的是 CMD=15/D0=2，与本板不符 */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)XIAOZHI_SD_PIN_CLK;
    slot.cmd = (gpio_num_t)XIAOZHI_SD_PIN_CMD;
    slot.d0 = (gpio_num_t)XIAOZHI_SD_PIN_D0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; /* 板载 CMD/D0 已有 10k，内部上拉仅兜底 */

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = (XIAOZHI_SD_FORMAT_IF_FAIL != 0),
        .max_files = XIAOZHI_SD_MAX_FILES,
        .allocation_unit_size = XIAOZHI_SD_ALLOC_UNIT,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(XIAOZHI_SD_MOUNT_POINT, &host, &slot, &mount_config, &card);
    if (err != ESP_OK || card == NULL) {
        /* 失败时 IDF 内部已释放 host/card；失败清理还会把 CLK/CMD/D0 复位成 GPIO 默认态，
         * 现场看到的 "I gpio: GPIO[14]| ... Pullup: 1" 就是这一步打出来的，属预期。 */
        s_fail_count++;
        if (xiaozhi_sdcard_freq_can_step_down()) {
            /* 降一档再试：本轮失败说明该时钟档读卡不稳，下一轮（守护任务 5s 后或下次显式 mount）降频 */
            s_freq_step++;
            ESP_LOGW(TAG, "mount failed at %d kHz (err=%s), step down to %d kHz on next try "
                          "(FRESULT 1 = disk I/O error, usually contact / signal integrity)",
                     freq_khz, esp_err_to_name(err), xiaozhi_sdcard_freq_khz(s_freq_step));
        } else if (s_fail_count == 1 || (s_fail_count % 12) == 0) {
            /* 已在最低档仍失败：日志节流，每 12 次（约 1 分钟）提示一次 */
            ESP_LOGW(TAG, "mount failed at lowest %d kHz (err=%s), retry every %d ms; check card / FAT32 / contact",
                     freq_khz, esp_err_to_name(err), XIAOZHI_SD_RETRY_MS);
        }
        xiaozhi_sdcard_unlock();
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    s_card = card;
    s_mounted = true;
    s_fail_count = 0;

    /* 卡信息（名称/类型/时钟）与容量，M1 验收要看这两行 */
    sdmmc_card_print_info(stdout, s_card);

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (xiaozhi_sdcard_stat(&total, &free_bytes)) {
        ESP_LOGI(TAG, "SD mounted at %s: total=%llu MB free=%llu MB, clock=%d kHz%s", XIAOZHI_SD_MOUNT_POINT,
                 (unsigned long long)(total / (1024 * 1024)), (unsigned long long)(free_bytes / (1024 * 1024)),
                 freq_khz, (s_freq_step > 0) ? " (fallback clock)" : "");
        s_cached_total = total;
        s_cached_free = free_bytes;
        s_cached_ok = true;
    } else {
        xiaozhi_sdcard_cache_invalidate();
    }

    xiaozhi_sdcard_prepare_dirs();
    xiaozhi_sdcard_unlock();
    return ESP_OK;
}

esp_err_t xiaozhi_sdcard_unmount(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    xiaozhi_sdcard_lock();
    /* 卸载/拔卡后时钟档位回到最高档：重新插卡时再按 最高→降频 的顺序探一次 */
    s_freq_step = 0;
    if (!s_mounted || s_card == NULL) {
        s_mounted = false;
        s_card = NULL;
        xiaozhi_sdcard_cache_invalidate();
        xiaozhi_sdcard_unlock();
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(XIAOZHI_SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmount failed (err=%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD unmounted");
    }
    /* 卸载失败也清状态：卡可能已被拔走，继续持有旧指针没有意义 */
    s_card = NULL;
    s_mounted = false;
    xiaozhi_sdcard_unlock();
    return err;
}

/* -------------------------------------------------------------------------- */
/* 状态与容量                                                                  */
/* -------------------------------------------------------------------------- */

bool xiaozhi_sdcard_ready(void)
{
    return s_mounted;
}

uint64_t xiaozhi_sdcard_total_bytes(void)
{
    uint64_t total = 0;
    if (!xiaozhi_sdcard_stat(&total, NULL)) {
        return 0;
    }
    return total;
}

uint64_t xiaozhi_sdcard_free_bytes(void)
{
    uint64_t free_bytes = 0;
    if (!xiaozhi_sdcard_stat(NULL, &free_bytes)) {
        return 0;
    }
    return free_bytes;
}

/* 非阻塞版本：只读缓存，供 HTTP 等不能做卡 I/O 的上下文使用 */
bool xiaozhi_sdcard_cache_ready(void)
{
    return s_cached_ok;
}

uint64_t xiaozhi_sdcard_free_bytes_cached(void)
{
    return s_cached_ok ? s_cached_free : 0;
}

uint64_t xiaozhi_sdcard_total_bytes_cached(void)
{
    return s_cached_ok ? s_cached_total : 0;
}

const char *xiaozhi_sdcard_root(void)
{
    return XIAOZHI_SD_MOUNT_POINT;
}

esp_err_t xiaozhi_sdcard_ensure_dir(const char *path)
{
    if (!s_mounted || path == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xiaozhi_sdcard_mkdir_if_absent(path);
}

void xiaozhi_sdcard_set_auto_recover(bool enable)
{
    s_auto_recover = enable;
}

/* -------------------------------------------------------------------------- */
/* 循环覆盖                                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t xiaozhi_sdcard_reclaim(uint64_t keep_free_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (!xiaozhi_sdcard_stat(&total, &free_bytes)) {
        return ESP_FAIL;
    }
    if (free_bytes >= keep_free_bytes) {
        return ESP_OK;
    }

    int deleted = 0;
    char oldest[XIAOZHI_SD_PATH_MAX];
    while (free_bytes < keep_free_bytes) {
        if (!xiaozhi_sdcard_find_oldest(XIAOZHI_SD_DCIM_DIR, oldest, sizeof(oldest), 0)) {
            ESP_LOGW(TAG, "reclaim: no more file to delete, free=%llu MB",
                     (unsigned long long)(free_bytes / (1024 * 1024)));
            break;
        }
        if (remove(oldest) != 0) {
            ESP_LOGW(TAG, "reclaim: remove %s failed (errno=%d)", oldest, errno);
            break;
        }
        deleted++;
        ESP_LOGI(TAG, "reclaim: delete %s", oldest);
        if (!xiaozhi_sdcard_stat(&total, &free_bytes)) {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "reclaim done: deleted=%d free=%llu MB (keep >= %llu MB)", deleted,
             (unsigned long long)(free_bytes / (1024 * 1024)),
             (unsigned long long)(keep_free_bytes / (1024 * 1024)));
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* 写速自检                                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t xiaozhi_sdcard_selftest(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 写速自检缓冲放 PSRAM（512KB 级别，不能占内部 RAM） */
    char *buf = (char *)heap_caps_malloc(XIAOZHI_SD_SELFTEST_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (char *)malloc(XIAOZHI_SD_SELFTEST_BYTES);
    }
    if (buf == NULL) {
        ESP_LOGW(TAG, "selftest: no memory for %d bytes", XIAOZHI_SD_SELFTEST_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0xA5, XIAOZHI_SD_SELFTEST_BYTES);

    char path[XIAOZHI_SD_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.sdtest.bin", XIAOZHI_SD_MOUNT_POINT);

    esp_err_t ret = ESP_OK;
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "selftest: open %s failed (errno=%d)", path, errno);
        free(buf);
        return ESP_FAIL;
    }

    int64_t t0 = esp_timer_get_time();
    size_t written = 0;
    while (written < XIAOZHI_SD_SELFTEST_BYTES) {
        size_t n = fwrite(buf + written, 1, XIAOZHI_SD_SELFTEST_CHUNK, fp);
        if (n != XIAOZHI_SD_SELFTEST_CHUNK) {
            ESP_LOGE(TAG, "selftest: write failed at %u (errno=%d)", (unsigned)written, errno);
            ret = ESP_FAIL;
            break;
        }
        written += n;
    }
    fclose(fp); /* 包含 flush，真正落盘时间算在速率里 */
    int64_t t1 = esp_timer_get_time();

    if (ret == ESP_OK) {
        int64_t us = t1 - t0;
        if (us <= 0) {
            us = 1;
        }
        uint32_t kbps = (uint32_t)(((uint64_t)written * 1000ULL) / (uint64_t)us);
        ESP_LOGI(TAG, "write test: %u KB in %lld ms => %u KB/s (%.2f MB/s)", (unsigned)(written / 1024),
                 (long long)(us / 1000), (unsigned)kbps, (double)kbps / 1024.0);
        if (kbps < 800) {
            ESP_LOGW(TAG, "write speed below 800 KB/s baseline, card may be slow or bus unstable");
        }
    }

    if (remove(path) != 0) {
        ESP_LOGW(TAG, "selftest: remove %s failed (errno=%d)", path, errno);
    }
    free(buf);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* 守护任务：拔卡检测 + 自动重挂载                                              */
/* -------------------------------------------------------------------------- */

#if XIAOZHI_SD_AUTO_MOUNT
static void xiaozhi_sdcard_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "monitor task started (period %d ms)", XIAOZHI_SD_RETRY_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_SD_RETRY_MS));

        if (!s_auto_recover) {
            continue;
        }

        if (!s_mounted) {
            /* 未挂载（上电无卡 / 卡被拔走 / 挂载失败）→ 周期重试 */
            int step_before = s_freq_step;
            if (xiaozhi_sdcard_mount() == ESP_OK) {
                ESP_LOGI(TAG, "remount ok");
            } else if (s_freq_step != step_before) {
                /* 刚降了一档时钟：不必再等满 5 s，立刻用新档位再试一次（卡在 20MHz
                 * 读不稳时可在 1 s 内挂上，而不是等 10 s 才退到最低档） */
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            continue;
        }

        /* 已挂载：用一次 fstatfs 判断卡是否还在（无 CD 脚，只能这样探），
         * 顺便刷新容量缓存（此处的阻塞只发生在守护任务，不影响 httpd） */
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        if (!xiaozhi_sdcard_stat(&total, &free_bytes)) {
            ESP_LOGW(TAG, "card lost, unmount and wait for reinsert");
            xiaozhi_sdcard_cache_invalidate();
            xiaozhi_sdcard_unmount();
        } else {
            s_cached_total = total;
            s_cached_free = free_bytes;
            s_cached_ok = true;
        }
    }
}
#endif /* XIAOZHI_SD_AUTO_MOUNT */

/* -------------------------------------------------------------------------- */
/* 初始化                                                                      */
/* -------------------------------------------------------------------------- */

esp_err_t xiaozhi_sdcard_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "create mutex failed");
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    s_auto_recover = true;

    esp_err_t err = ESP_OK;

#if XIAOZHI_SD_AUTO_MOUNT
    err = xiaozhi_sdcard_mount(); /* 失败不阻塞启动，守护任务会继续重试 */

#if XIAOZHI_SD_SPEED_TEST
    if (err == ESP_OK) {
        xiaozhi_sdcard_selftest();
    }
#endif

    /* 栈放 PSRAM（xiaozhi_task_create 统一处理，失败自动回落内部 RAM）。
     * 本任务仅做 SDMMC/FATFS 查询与挂载，不碰 flash 写，PSRAM 栈安全。 */
    if (xiaozhi_task_create(xiaozhi_sdcard_task, "sd_monitor", XIAOZHI_STACK_4K, NULL, XIAOZHI_PRIO_LOWEST, NULL,
                            XIAOZHI_SD_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create monitor task failed");
    }
#else
    ESP_LOGI(TAG, "init ok (auto mount disabled)");
#endif

    return err;
}

#else /* XIAOZHI_SD_ENABLE == 0：空实现，不占用 SDMMC、不建任务 */

esp_err_t xiaozhi_sdcard_init(void)
{
    return ESP_OK;
}
esp_err_t xiaozhi_sdcard_mount(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_sdcard_unmount(void)
{
    return ESP_OK;
}
bool xiaozhi_sdcard_ready(void)
{
    return false;
}
uint64_t xiaozhi_sdcard_total_bytes(void)
{
    return 0;
}
uint64_t xiaozhi_sdcard_free_bytes(void)
{
    return 0;
}
bool xiaozhi_sdcard_cache_ready(void)
{
    return false;
}
uint64_t xiaozhi_sdcard_free_bytes_cached(void)
{
    return 0;
}
uint64_t xiaozhi_sdcard_total_bytes_cached(void)
{
    return 0;
}
esp_err_t xiaozhi_sdcard_ensure_dir(const char *path)
{
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_sdcard_reclaim(uint64_t keep_free_bytes)
{
    (void)keep_free_bytes;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_sdcard_selftest(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
const char *xiaozhi_sdcard_root(void)
{
    return XIAOZHI_SD_MOUNT_POINT;
}
void xiaozhi_sdcard_set_auto_recover(bool enable)
{
    (void)enable;
}

#endif /* XIAOZHI_SD_ENABLE */
