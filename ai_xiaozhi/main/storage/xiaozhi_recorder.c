#include "xiaozhi_recorder.h"

#include "xiaozhi_common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "xiaozhi_appmode.h"
#include "xiaozhi_camera.h"
#include "xiaozhi_camview.h"
#include "xiaozhi_doorbell.h"
#include "xiaozhi_sdcard.h"

#define TAG "recorder"

#if XIAOZHI_REC_ENABLE

/* ==========================================================================
 * AVI 头布局（MJPEG，单视频流），全部偏移基于文件起始的绝对位置：
 *
 *   0 'RIFF' | 4 size | 8 'AVI '
 *  12 'LIST' | 16 192 | 20 'hdrl'
 *  24 'avih' | 28 56   | 32  avih 数据（56B，32..87）
 *  88 'LIST' | 92 116  | 96 'strl'
 * 100 'strh' |104 56   |108  strh 数据（56B，108..163）
 * 164 'strf' |168 40   |172  strf 数据（40B，172..211）
 * 212 'LIST' |216 size |220 'movi'      <- idx1 的 offset 以 220（'movi' 四字符码）为基准
 * 224 第一个 '00dc' 块
 * ========================================================================== */
#define AVI_HDR_SIZE         224
#define AVI_RIFF_SIZE_POS    4
#define AVI_AVIH_USEC_POS    32
#define AVI_AVIH_MAXBPS_POS  36
#define AVI_AVIH_FLAGS_POS   44
#define AVI_AVIH_FRAMES_POS  48
#define AVI_AVIH_SUGGEST_POS 60
#define AVI_AVIH_W_POS       64
#define AVI_AVIH_H_POS       68
#define AVI_STRH_LEN_POS     140
#define AVI_STRH_SUGGEST_POS 144
#define AVI_STRF_W_POS       176
#define AVI_STRF_H_POS       180
#define AVI_MOVI_SIZE_POS    216
#define AVI_MOVI_BASE        220

#define AVI_FLAG_HASINDEX    0x00000010u

/* 索引缓冲区上限（PSRAM，12 字节/帧 -> 这里存的是 8 字节/帧） */
typedef struct {
    FILE *fp;
    int w;
    int h;
    int fps;
    uint32_t frames;      /* 已写帧数 */
    uint32_t movi_bytes;  /* movi 数据区字节（chunk header + data + padding） */
    uint32_t max_frame;   /* 最大单帧字节，回填 dwSuggestedBufferSize */
    uint32_t idx_cap;
    uint32_t *idx_off;    /* 每帧 chunk 头相对 'movi' 四字符码的偏移 */
    uint32_t *idx_size;   /* 每帧字节数 */
    int64_t last_sync_us;
} rec_avi_t;

/* ---------------- 模块状态 ---------------- */

static bool s_inited = false;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;

typedef struct {
    uint8_t *buf;
    size_t cap;
    volatile bool busy;
} rec_pool_slot_t;

static rec_pool_slot_t s_pool[XIAOZHI_REC_POOL_FRAMES];

typedef struct {
    int slot;
    size_t len;
    int w;
    int h;
} rec_frame_t;

/* 录制意图（由 start/stop 设置，worker 消费） */
static volatile bool s_want = false;
static bool s_event = false;
static volatile int64_t s_deadline_us = 0;
static volatile int64_t s_retry_at_us = 0;
static char s_prefix[32] = "REC";
/* true = s_prefix 已经是完整文件名主体（门铃事件 ID，自带 EVT_<时间>），
 * false = s_prefix 只是前缀，命名时还要拼时间戳（连续录像 "REC_<时间>"）。 */
static bool s_base_from_id = false;
static uint32_t s_slice_ms = XIAOZHI_REC_SLICE_MS;

/* 当前文件 */
static rec_avi_t s_avi;
static volatile bool s_file_open = false;
static volatile xiaozhi_rec_state_t s_state = XIAOZHI_REC_STATE_IDLE;
static int64_t s_slice_start_us = 0;
static int64_t s_space_check_us = 0;
static int64_t s_last_write_us = 0;

/* 会话统计 */
static uint32_t s_session_files = 0;
static uint64_t s_session_bytes = 0;
static uint32_t s_dropped = 0;
static int64_t s_win_start_us = 0;
static uint32_t s_win_frames = 0;
static float s_fps = 0.0f;
static char s_reason[48] = "";
static char s_file_rel[80] = "";
static char s_file_full[192] = "";

/* 资源归属 */
static bool s_tap_registered = false;
static bool s_cam_owned = false;

/* 帧间隔（微秒） */
static int64_t s_frame_gap_us = 1000000 / XIAOZHI_REC_FPS;

/* -------------------------------------------------------------------------- */
/* 小工具                                                                      */
/* -------------------------------------------------------------------------- */

static void rec_put_u32(uint8_t **p, uint32_t v)
{
    memcpy(*p, &v, 4);
    *p += 4;
}

static void rec_put_u16(uint8_t **p, uint16_t v)
{
    memcpy(*p, &v, 2);
    *p += 2;
}

static void rec_put_fourcc(uint8_t **p, const char *fourcc)
{
    memcpy(*p, fourcc, 4);
    *p += 4;
}

static void rec_set_reason(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_reason, sizeof(s_reason), fmt, ap);
    va_end(ap);
}

/* 循环覆盖保留水位：大卡用 XIAOZHI_REC_KEEP_FREE_BYTES，
 * 小卡自动缩到容量的 1/4，避免"一开新片就把卡删空" */
static uint64_t rec_keep_free_target(void)
{
    uint64_t total = xiaozhi_sdcard_total_bytes();
    uint64_t keep = XIAOZHI_REC_KEEP_FREE_BYTES;
    if (total > 0 && keep > total / 4) {
        keep = total / 4;
    }
    uint64_t floor_keep = XIAOZHI_REC_MIN_FREE_BYTES * 2ULL;
    if (keep < floor_keep) {
        keep = floor_keep;
    }
    return keep;
}

/* -------------------------------------------------------------------------- */
/* AVI 写文件                                                                  */
/* -------------------------------------------------------------------------- */

static esp_err_t avi_open(rec_avi_t *a, const char *path, uint32_t max_frames)
{
    memset(a, 0, sizeof(*a));
    a->w = XIAOZHI_REC_W;
    a->h = XIAOZHI_REC_H;
    a->fps = XIAOZHI_REC_FPS;
    a->idx_cap = max_frames;

    a->idx_off = (uint32_t *)heap_caps_calloc(max_frames, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a->idx_size = (uint32_t *)heap_caps_calloc(max_frames, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (a->idx_off == NULL || a->idx_size == NULL) {
        ESP_LOGE(TAG, "no psram for avi index (%u frames)", (unsigned)max_frames);
        goto fail;
    }

    a->fp = fopen(path, "wb");
    if (a->fp == NULL) {
        ESP_LOGE(TAG, "open %s failed (errno=%d)", path, errno);
        goto fail;
    }

    uint8_t hdr[AVI_HDR_SIZE];
    uint8_t *p = hdr;
    rec_put_fourcc(&p, "RIFF");
    rec_put_u32(&p, 0);           /* RIFF size，关闭/同步时回填 */
    rec_put_fourcc(&p, "AVI ");
    rec_put_fourcc(&p, "LIST");
    rec_put_u32(&p, 192);         /* hdrl 长度 = 4 + (8+56) + (8+116) */
    rec_put_fourcc(&p, "hdrl");
    rec_put_fourcc(&p, "avih");
    rec_put_u32(&p, 56);
    rec_put_u32(&p, 1000000u / (uint32_t)a->fps); /* dwMicroSecPerFrame */
    rec_put_u32(&p, 0);           /* dwMaxBytesPerSec */
    rec_put_u32(&p, 0);           /* dwPaddingGranularity */
    rec_put_u32(&p, AVI_FLAG_HASINDEX);
    rec_put_u32(&p, 0);           /* dwTotalFrames */
    rec_put_u32(&p, 0);           /* dwInitialFrames */
    rec_put_u32(&p, 1);           /* dwStreams */
    rec_put_u32(&p, 0);           /* dwSuggestedBufferSize */
    rec_put_u32(&p, (uint32_t)a->w);
    rec_put_u32(&p, (uint32_t)a->h);
    for (int i = 0; i < 4; i++) {
        rec_put_u32(&p, 0);       /* dwReserved[4] */
    }
    rec_put_fourcc(&p, "LIST");
    rec_put_u32(&p, 116);         /* strl 长度 = 4 + (8+56) + (8+40) */
    rec_put_fourcc(&p, "strl");
    rec_put_fourcc(&p, "strh");
    rec_put_u32(&p, 56);
    rec_put_fourcc(&p, "vids");
    rec_put_fourcc(&p, "MJPG");
    rec_put_u32(&p, 0);           /* dwFlags */
    rec_put_u16(&p, 0);           /* wPriority */
    rec_put_u16(&p, 0);           /* wLanguage */
    rec_put_u32(&p, 0);           /* dwInitialFrames */
    rec_put_u32(&p, 1);           /* dwScale */
    rec_put_u32(&p, (uint32_t)a->fps); /* dwRate */
    rec_put_u32(&p, 0);           /* dwStart */
    rec_put_u32(&p, 0);           /* dwLength */
    rec_put_u32(&p, 0);           /* dwSuggestedBufferSize */
    rec_put_u32(&p, 0xFFFFFFFFu); /* dwQuality */
    rec_put_u32(&p, 0);           /* dwSampleSize */
    rec_put_u16(&p, 0);           /* rcFrame.left */
    rec_put_u16(&p, 0);           /* rcFrame.top */
    rec_put_u16(&p, (uint16_t)a->w);
    rec_put_u16(&p, (uint16_t)a->h);
    rec_put_fourcc(&p, "strf");
    rec_put_u32(&p, 40);
    rec_put_u32(&p, 40);          /* biSize */
    rec_put_u32(&p, (uint32_t)a->w);
    rec_put_u32(&p, (uint32_t)a->h);
    rec_put_u16(&p, 1);           /* biPlanes */
    rec_put_u16(&p, 24);          /* biBitCount */
    rec_put_fourcc(&p, "MJPG");   /* biCompression */
    rec_put_u32(&p, (uint32_t)(a->w * a->h * 3));
    rec_put_u32(&p, 0);           /* biXPelsPerMeter */
    rec_put_u32(&p, 0);           /* biYPelsPerMeter */
    rec_put_u32(&p, 0);           /* biClrUsed */
    rec_put_u32(&p, 0);           /* biClrImportant */
    rec_put_fourcc(&p, "LIST");
    rec_put_u32(&p, 4);           /* movi 长度（先写 4 = 只含 'movi'），回填时改 */
    rec_put_fourcc(&p, "movi");

    if ((size_t)(p - hdr) != AVI_HDR_SIZE) {
        ESP_LOGE(TAG, "avi header layout error: %u != %u", (unsigned)(p - hdr), (unsigned)AVI_HDR_SIZE);
        goto fail;
    }

    if (fwrite(hdr, 1, sizeof(hdr), a->fp) != sizeof(hdr)) {
        ESP_LOGE(TAG, "write header failed (errno=%d)", errno);
        goto fail;
    }

    a->last_sync_us = esp_timer_get_time();
    return ESP_OK;

fail:
    if (a->fp != NULL) {
        fclose(a->fp);
        a->fp = NULL;
    }
    if (a->idx_off != NULL) {
        heap_caps_free(a->idx_off);
        a->idx_off = NULL;
    }
    if (a->idx_size != NULL) {
        heap_caps_free(a->idx_size);
        a->idx_size = NULL;
    }
    return ESP_FAIL;
}

/* 回填头部（final=true 表示已写完 idx1，可以声明 HASINDEX） */
static esp_err_t avi_patch(rec_avi_t *a, bool final)
{
    if (a->fp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    long end = ftell(a->fp);
    if (end < AVI_HDR_SIZE) {
        return ESP_FAIL;
    }

    const uint32_t frames = a->frames;
    const uint32_t movi_size = a->movi_bytes + 4; /* 4 = 'movi' 四字符码 */
    const uint32_t riff_size = (uint32_t)(end - 8);
    const uint32_t usec = (a->fps > 0) ? (1000000u / (uint32_t)a->fps) : 40000u;
    const uint32_t dur_s = (uint32_t)(((uint64_t)usec * frames) / 1000000ULL);
    const uint32_t max_bps = (dur_s > 0) ? (uint32_t)((uint64_t)a->movi_bytes / dur_s) : 0;
    const uint32_t flags = final ? AVI_FLAG_HASINDEX : 0u;

    const struct {
        long pos;
        uint32_t val;
    } fields[] = {
        {AVI_RIFF_SIZE_POS, riff_size},
        {AVI_AVIH_USEC_POS, usec},
        {AVI_AVIH_MAXBPS_POS, max_bps},
        {AVI_AVIH_FLAGS_POS, flags},
        {AVI_AVIH_FRAMES_POS, frames},
        {AVI_AVIH_SUGGEST_POS, a->max_frame},
        {AVI_AVIH_W_POS, (uint32_t)a->w},
        {AVI_AVIH_H_POS, (uint32_t)a->h},
        {AVI_STRH_LEN_POS, frames},
        {AVI_STRH_SUGGEST_POS, a->max_frame},
        {AVI_STRF_W_POS, (uint32_t)a->w},
        {AVI_STRF_H_POS, (uint32_t)a->h},
        {AVI_MOVI_SIZE_POS, movi_size},
    };

    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fseek(a->fp, fields[i].pos, SEEK_SET) != 0 || fwrite(&fields[i].val, 1, 4, a->fp) != 4) {
            ret = ESP_FAIL;
            break;
        }
    }
    /* 无论成败都尽量回到文件尾，避免把后续帧写进头部 */
    if (fseek(a->fp, 0, SEEK_END) != 0) {
        ret = ESP_FAIL;
    }
    return ret;
}

/* 周期落盘：回填头 + fsync，掉电后文件仍可播 */
static esp_err_t avi_sync(rec_avi_t *a)
{
    if (a->fp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = avi_patch(a, false);
    if (fflush(a->fp) != 0) {
        ret = ESP_FAIL;
    }
    if (fsync(fileno(a->fp)) != 0) {
        ret = ESP_FAIL;
    }
    a->last_sync_us = esp_timer_get_time();
    return ret;
}

static esp_err_t avi_write_frame(rec_avi_t *a, const uint8_t *buf, size_t len)
{
    if (a->fp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0 || a->frames >= a->idx_cap) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t ch[8];
    uint32_t n = (uint32_t)len;
    memcpy(ch, "00dc", 4);
    memcpy(ch + 4, &n, 4);

    if (fwrite(ch, 1, sizeof(ch), a->fp) != sizeof(ch)) {
        return ESP_FAIL;
    }
    if (fwrite(buf, 1, len, a->fp) != len) {
        return ESP_FAIL;
    }
    uint32_t pad = n & 1u;
    if (pad != 0) {
        uint8_t zero = 0;
        if (fwrite(&zero, 1, 1, a->fp) != 1) {
            return ESP_FAIL;
        }
    }

    a->idx_off[a->frames] = a->movi_bytes + 4;
    a->idx_size[a->frames] = n;
    a->frames++;
    a->movi_bytes += 8 + n + pad;
    if (n > a->max_frame) {
        a->max_frame = n;
    }
    return ESP_OK;
}

/* 关闭文件：写 idx1 → 回填头 → fsync → fclose → 释放索引 */
static esp_err_t avi_close(rec_avi_t *a)
{
    if (a->fp == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (a->frames > 0) {
        const uint32_t n = a->frames;
        uint8_t head[8];
        uint32_t size = n * 16u;
        memcpy(head, "idx1", 4);
        memcpy(head + 4, &size, 4);
        if (fwrite(head, 1, sizeof(head), a->fp) != sizeof(head)) {
            ret = ESP_FAIL;
        }
        for (uint32_t i = 0; i < n && ret == ESP_OK; i++) {
            uint8_t ent[16];
            uint32_t flags = AVI_FLAG_HASINDEX;
            memcpy(ent + 0, "00dc", 4);
            memcpy(ent + 4, &flags, 4);
            memcpy(ent + 8, &a->idx_off[i], 4);
            memcpy(ent + 12, &a->idx_size[i], 4);
            if (fwrite(ent, 1, sizeof(ent), a->fp) != sizeof(ent)) {
                ret = ESP_FAIL;
            }
        }
    }

    if (avi_patch(a, a->frames > 0) != ESP_OK) {
        ret = ESP_FAIL;
    }
    if (fflush(a->fp) != 0) {
        ret = ESP_FAIL;
    }
    if (fsync(fileno(a->fp)) != 0) {
        ret = ESP_FAIL;
    }
    if (fclose(a->fp) != 0) {
        ret = ESP_FAIL;
    }
    a->fp = NULL;

    if (a->idx_off != NULL) {
        heap_caps_free(a->idx_off);
        a->idx_off = NULL;
    }
    if (a->idx_size != NULL) {
        heap_caps_free(a->idx_size);
        a->idx_size = NULL;
    }
    return ret;
}

/* -------------------------------------------------------------------------- */
/* 文件命名                                                                    */
/* -------------------------------------------------------------------------- */

/* 生成目录 + 文件路径：/sdcard/DCIM/<日期>/<文件名主体>.avi
 *   连续录像：REC_<YYYYMMDD_HHMMSS>.avi
 *   事件录像：直接沿用门铃事件 ID（EVT_<YYYYMMDD_HHMMSS>.avi），不再拼时间戳
 * 未授时落 /sdcard/DCIM/BOOT/，绝不写 1970 的假时间 */
static void rec_build_path(char *dir, size_t dir_len, char *path, size_t path_len)
{
    char base[64];
    time_t now = time(NULL);
    struct tm tm_now;
    bool synced = (now >= 1700000000) && (localtime_r(&now, &tm_now) != NULL);

    if (synced) {
        char day[16];
        strftime(day, sizeof(day), "%Y-%m-%d", &tm_now);
        snprintf(dir, dir_len, "%s/DCIM/%s", XIAOZHI_SD_MOUNT_POINT, day);
    } else {
        snprintf(dir, dir_len, "%s/DCIM/BOOT", XIAOZHI_SD_MOUNT_POINT);
    }

    if (s_base_from_id) {
        /* 事件录像：s_prefix 是门铃的 event_id（EVT_YYYYMMDD_HHMMSS 或 EVT_BOOT_<秒>），
         * 本身已含时间，再拼一次就会得到
         * EVT_20261106_191500_20261106_191500.avi 这种重复文件名 */
        snprintf(base, sizeof(base), "%s", s_prefix);
    } else if (synced) {
        char ts[24];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);
        snprintf(base, sizeof(base), "%s_%s", s_prefix, ts);
    } else {
        snprintf(base, sizeof(base), "%s_BOOT_%lu", s_prefix,
                 (unsigned long)(esp_timer_get_time() / 1000000));
    }

    /* 同秒重开 / 重启后重名时加序号，避免覆盖已有录像 */
    struct stat st;
    path[0] = '\0';
    for (int i = 0; i < 100; i++) {
        if (i == 0) {
            snprintf(path, path_len, "%s/%s.avi", dir, base);
        } else {
            snprintf(path, path_len, "%s/%s_%02d.avi", dir, base, i);
        }
        if (stat(path, &st) != 0) {
            break; /* 不存在，可用 */
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 帧缓冲池 / tap 回调                                                          */
/* -------------------------------------------------------------------------- */

static int rec_pool_acquire(void)
{
    for (int i = 0; i < XIAOZHI_REC_POOL_FRAMES; i++) {
        if (!s_pool[i].busy && s_pool[i].buf != NULL) {
            s_pool[i].busy = true;
            return i;
        }
    }
    return -1;
}

/* camview 预览任务的帧回调：只做"节流 + 拷贝 + 入队"，绝不写盘 */
static void rec_on_frame(const uint8_t *jpeg, size_t len, int w, int h, void *ctx)
{
    (void)ctx;
    if (!s_want || !s_file_open) {
        return;
    }
    if (len == 0 || len > XIAOZHI_REC_FRAME_MAX_BYTES) {
        s_dropped++;
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s_last_write_us != 0 && (now - s_last_write_us) < s_frame_gap_us) {
        return; /* 预览 10fps → 录像 8fps，多余的帧直接丢 */
    }

    int slot = rec_pool_acquire();
    if (slot < 0) {
        s_dropped++;
        return;
    }

    memcpy(s_pool[slot].buf, jpeg, len);
    rec_frame_t f = {
        .slot = slot,
        .len = len,
        .w = w,
        .h = h,
    };
    s_last_write_us = now;
    if (xQueueSend(s_queue, &f, 0) != pdTRUE) {
        s_pool[slot].busy = false;
        s_dropped++;
    }
}

/* -------------------------------------------------------------------------- */
/* 状态迁移                                                                    */
/* -------------------------------------------------------------------------- */

static void rec_close_file(void)
{
    if (!s_file_open) {
        s_state = XIAOZHI_REC_STATE_IDLE;
        return;
    }
    s_file_open = false;

    uint32_t frames = s_avi.frames;
    uint32_t bytes = s_avi.movi_bytes;
    esp_err_t err = avi_close(&s_avi);

    if (frames == 0 && s_file_full[0] != '\0') {
        /* 一帧都没写进去（开片后立刻停/立刻掉盘）：删掉空文件，别留垃圾 */
        if (remove(s_file_full) != 0) {
            ESP_LOGW(TAG, "remove empty file %s failed (errno=%d)", s_file_full, errno);
        } else {
            ESP_LOGI(TAG, "empty file removed: %s", s_file_rel);
            if (s_session_files > 0) {
                s_session_files--;
            }
        }
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "file close reported error (card removed?), %s", s_file_rel);
    }

    if (frames > 0) {
        ESP_LOGI(TAG, "file closed: %s (%u frames, %u KB)%s", s_file_rel, (unsigned)frames,
                 (unsigned)(bytes / 1024), (err == ESP_OK) ? "" : " [incomplete]");
    }

    s_state = XIAOZHI_REC_STATE_IDLE;
    s_retry_at_us = 0;
    s_file_rel[0] = '\0';
    s_file_full[0] = '\0';
}

static void rec_degrade(const char *reason)
{
    rec_set_reason("%s", reason);
    ESP_LOGW(TAG, "degrade: %s -> wait for sd recovery (%d ms)", reason, XIAOZHI_REC_RETRY_MS);

    if (s_file_open) {
        /* 盘已经不可用：尽量把文件尾巴收好（失败也无所谓），然后主动卸载，
         * 让 sd_monitor 任务按"拔卡"流程重挂载 */
        rec_close_file();
    }
    s_state = XIAOZHI_REC_STATE_NO_SD;
    s_retry_at_us = esp_timer_get_time() + (int64_t)XIAOZHI_REC_RETRY_MS * 1000;
    if (xiaozhi_sdcard_ready()) {
        (void)xiaozhi_sdcard_unmount();
    }
}

static void rec_try_open(void)
{
    if (!xiaozhi_sdcard_ready()) {
        if (s_state != XIAOZHI_REC_STATE_NO_SD) {
            rec_set_reason("sd not mounted");
            ESP_LOGW(TAG, "SD not mounted, recording paused");
        }
        s_state = XIAOZHI_REC_STATE_NO_SD;
        s_retry_at_us = esp_timer_get_time() + (int64_t)XIAOZHI_REC_RETRY_MS * 1000;
        return;
    }

    /* 卡满保护：先循环删除 DCIM 里最旧的文件 */
    /* 循环覆盖：每次开新片前检查保留水位（不是等到卡快满才一次性大删，
     * 这样每片只需删掉 1~2 个最旧文件，写盘更平稳） */
    uint64_t keep = rec_keep_free_target();
    uint64_t free_bytes = xiaozhi_sdcard_free_bytes();
    if (free_bytes < keep) {
        ESP_LOGW(TAG, "free %llu KB < keep %llu KB, delete oldest in DCIM",
                 (unsigned long long)(free_bytes / 1024), (unsigned long long)(keep / 1024));
        (void)xiaozhi_sdcard_reclaim(keep);
        free_bytes = xiaozhi_sdcard_free_bytes();
        if (free_bytes < XIAOZHI_REC_MIN_FREE_BYTES) {
            rec_set_reason("card full");
            s_state = XIAOZHI_REC_STATE_NO_SPACE;
            s_retry_at_us = esp_timer_get_time() + (int64_t)XIAOZHI_REC_RETRY_MS * 1000;
            ESP_LOGW(TAG, "still only %llu KB free, recording paused",
                     (unsigned long long)(free_bytes / 1024));
            return;
        }
    }

    char dir[96];
    char path[176];
    rec_build_path(dir, sizeof(dir), path, sizeof(path));

    if (xiaozhi_sdcard_ensure_dir(dir) != ESP_OK) {
        rec_degrade("mkdir failed");
        return;
    }

    /* 索引容量按"本片最长时长 × 帧率"算，多留 16 帧余量 */
    uint32_t cap = (uint32_t)(((uint64_t)s_slice_ms * XIAOZHI_REC_FPS) / 1000ULL) + 16;
    if (cap < 32) {
        cap = 32;
    }

    if (avi_open(&s_avi, path, cap) != ESP_OK) {
        rec_degrade("open failed");
        return;
    }

    s_slice_start_us = esp_timer_get_time();
    s_space_check_us = s_slice_start_us;
    s_last_write_us = 0;
    s_file_open = true;
    s_state = XIAOZHI_REC_STATE_RECORDING;
    s_retry_at_us = 0;
    s_session_files++;
    s_win_start_us = s_slice_start_us;
    s_win_frames = 0;
    s_reason[0] = '\0';

    /* 状态用相对路径（去掉挂载点前缀），方便网页直接下载 */
    snprintf(s_file_full, sizeof(s_file_full), "%s", path);
    const char *rel = path;
    size_t root_len = strlen(XIAOZHI_SD_MOUNT_POINT);
    if (strncmp(path, XIAOZHI_SD_MOUNT_POINT, root_len) == 0) {
        rel = path + root_len;
    }
    strncpy(s_file_rel, rel, sizeof(s_file_rel) - 1);
    s_file_rel[sizeof(s_file_rel) - 1] = '\0';

    ESP_LOGI(TAG, "record start: %s (cap=%u frames, %d fps, slice=%u ms)", path, (unsigned)cap,
             XIAOZHI_REC_FPS, (unsigned)s_slice_ms);
}

/* -------------------------------------------------------------------------- */
/* 写帧                                                                        */
/* -------------------------------------------------------------------------- */

static void rec_write_frame(const uint8_t *buf, size_t len, int w, int h)
{
    if (!s_file_open) {
        return;
    }
    if (len == 0 || len > XIAOZHI_REC_FRAME_MAX_BYTES) {
        s_dropped++;
        return;
    }

    /* 事件录像（自己 grab）时在这里节流；监控模式由 tap 节流 */
    if (s_cam_owned) {
        int64_t now = esp_timer_get_time();
        if (s_last_write_us != 0 && (now - s_last_write_us) < s_frame_gap_us) {
            return;
        }
        s_last_write_us = now;
    }

    if (s_avi.frames == 0 && w > 0 && h > 0) {
        s_avi.w = w;
        s_avi.h = h;
    }

    esp_err_t err = avi_write_frame(&s_avi, buf, len);
    if (err == ESP_ERR_INVALID_SIZE) {
        /* 索引满（等于切片到点），交给主循环切片处理 */
        ESP_LOGW(TAG, "index full (%u frames), roll over", (unsigned)s_avi.frames);
        rec_close_file();
        return;
    }
    if (err != ESP_OK) {
        rec_degrade("write failed");
        return;
    }

    s_session_bytes += len;
    s_win_frames++;

    /* 每 SYNC_MS 回填头 + fsync；顺便统计实测帧率 */
    int64_t now = esp_timer_get_time();
    if ((now - s_avi.last_sync_us) >= (int64_t)XIAOZHI_REC_SYNC_MS * 1000) {
        if (avi_sync(&s_avi) != ESP_OK) {
            rec_degrade("sync failed");
            return;
        }
        int64_t win_us = now - s_win_start_us;
        if (win_us >= 3000000) {
            s_fps = (float)s_win_frames * 1000000.0f / (float)win_us;
            ESP_LOGI(TAG, "%.1f fps, session %u files / %llu KB / drop %u", s_fps, (unsigned)s_session_files,
                     (unsigned long long)(s_session_bytes / 1024), (unsigned)s_dropped);
            s_win_start_us = now;
            s_win_frames = 0;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 停止清理                                                                    */
/* -------------------------------------------------------------------------- */

static void rec_release_resources(void)
{
    if (s_tap_registered) {
        (void)xiaozhi_camview_set_frame_tap(XIAOZHI_CAMVIEW_TAP_RECORDER, NULL, NULL);
        s_tap_registered = false;
    }
    if (s_cam_owned) {
        s_cam_owned = false;
        /* 只有文件已收尾才 deinit，避免与 worker 里的 grab 打架。
         * XIAOZHI_CAM_KEEP_INIT=1 时保持摄像头初始化（本板无 PWDN/RESET，
         * deinit 后再 init 会 SCCB 超时，见 xiaozhi_camera.h 说明）。 */
        if (!s_file_open && xiaozhi_camera_ready() && !xiaozhi_camview_is_open()) {
#if !XIAOZHI_CAM_KEEP_INIT
            (void)xiaozhi_camera_deinit();
            ESP_LOGI(TAG, "own camera released");
#else
            ESP_LOGI(TAG, "own camera kept initialized (XIAOZHI_CAM_KEEP_INIT=1)");
#endif
        }
    }
    s_event = false;
    s_deadline_us = 0;
}

/* -------------------------------------------------------------------------- */
/* 录像任务                                                                    */
/* -------------------------------------------------------------------------- */

static void rec_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task started: fps=%d slice=%u ms sync=%d ms", XIAOZHI_REC_FPS, (unsigned)s_slice_ms,
             XIAOZHI_REC_SYNC_MS);

    rec_frame_t f;

    for (;;) {
        bool got = (xQueueReceive(s_queue, &f, pdMS_TO_TICKS(50)) == pdTRUE);
        int64_t now = esp_timer_get_time();

        /* 1. 未要求录像：收尾并清空残留帧 */
        if (!s_want) {
            if (s_file_open) {
                rec_close_file();
            } else if (s_state != XIAOZHI_REC_STATE_IDLE) {
                s_state = XIAOZHI_REC_STATE_IDLE;
            }
            if (got) {
                s_pool[f.slot].busy = false;
            }
            while (xQueueReceive(s_queue, &f, 0) == pdTRUE) {
                s_pool[f.slot].busy = false;
            }
            continue;
        }

        /* 2. 事件录像到点：关文件、释放摄像头 */
        if (s_event && s_deadline_us > 0 && now >= s_deadline_us) {
            ESP_LOGI(TAG, "event record finished (%u files, %llu KB)", (unsigned)s_session_files,
                     (unsigned long long)(s_session_bytes / 1024));
            if (s_file_open) {
                rec_close_file();
            }
            s_want = false;
            rec_release_resources();
            if (got) {
                s_pool[f.slot].busy = false;
            }
            continue;
        }

        /* 3. 没在写文件（启动/切片交接/掉盘/卡满）→ 到点尝试开新片 */
        if (s_state != XIAOZHI_REC_STATE_RECORDING && now >= s_retry_at_us) {
            rec_try_open();
        }

        if (s_state == XIAOZHI_REC_STATE_RECORDING) {
            /* 3.1 切片到点：关掉本片，下一轮立刻开新片 */
            if ((now - s_slice_start_us) >= (int64_t)s_slice_ms * 1000) {
                ESP_LOGI(TAG, "slice time reached, roll over");
                rec_close_file();
                if (got) {
                    s_pool[f.slot].busy = false;
                }
                continue;
            }

            /* 3.2 空间巡检：低于硬阈值就切下一片（开新片时会 reclaim） */
            if ((now - s_space_check_us) >= (int64_t)XIAOZHI_REC_SPACE_CHECK_MS * 1000) {
                s_space_check_us = now;
                if (!xiaozhi_sdcard_ready()) {
                    rec_degrade("card lost");
                    if (got) {
                        s_pool[f.slot].busy = false;
                    }
                    continue;
                }
                if (xiaozhi_sdcard_free_bytes() < XIAOZHI_REC_MIN_FREE_BYTES) {
                    ESP_LOGW(TAG, "low space, roll over to reclaim");
                    rec_close_file();
                    if (got) {
                        s_pool[f.slot].busy = false;
                    }
                    continue;
                }
            }

            /* 3.3 写一帧：监控模式来自 tap 队列，事件录像自己 grab */
            if (got) {
                rec_write_frame(s_pool[f.slot].buf, f.len, f.w, f.h);
                s_pool[f.slot].busy = false;
            } else if (s_cam_owned) {
                camera_fb_t *fb = xiaozhi_camera_grab();
                if (fb != NULL) {
                    if (fb->format == PIXFORMAT_JPEG) {
                        rec_write_frame(fb->buf, fb->len, fb->width, fb->height);
                    } else {
                        s_dropped++;
                    }
                    xiaozhi_camera_return(fb);
                }
            }
        } else if (got) {
            s_pool[f.slot].busy = false;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 模式钩子 / 门铃后端                                                          */
/* -------------------------------------------------------------------------- */

static void rec_mode_monitor_enter(xiaozhi_mode_t mode, xiaozhi_mode_src_t src, void *ctx)
{
    (void)mode;
    (void)ctx;
#if XIAOZHI_REC_MONITOR_CONTINUOUS
    if (!xiaozhi_camera_ready()) {
        ESP_LOGW(TAG, "monitor enter (src=%d) but camera not ready, skip record", (int)src);
        return;
    }
    esp_err_t err = xiaozhi_recorder_start_continuous();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start continuous record failed: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGI(TAG, "monitor enter (src=%d), continuous record disabled", (int)src);
#endif
}

static void rec_mode_monitor_exit(xiaozhi_mode_t mode, void *ctx)
{
    (void)mode;
    (void)ctx;
    xiaozhi_recorder_stop();
}

#if XIAOZHI_REC_EVENT_ENABLE
static esp_err_t rec_doorbell_start(const char *event_id, uint32_t duration_ms, void *ctx)
{
    (void)ctx;
    return xiaozhi_recorder_start_event(event_id, duration_ms);
}

static void rec_doorbell_stop(void *ctx)
{
    (void)ctx;
    xiaozhi_recorder_stop();
}
#endif

/* -------------------------------------------------------------------------- */
/* 对外 API                                                                    */
/* -------------------------------------------------------------------------- */

const char *xiaozhi_recorder_state_name(xiaozhi_rec_state_t state)
{
    switch (state) {
        case XIAOZHI_REC_STATE_RECORDING:
            return "recording";
        case XIAOZHI_REC_STATE_NO_SD:
            return "no_sd";
        case XIAOZHI_REC_STATE_NO_SPACE:
            return "no_space";
        case XIAOZHI_REC_STATE_IDLE:
        default:
            return "idle";
    }
}

esp_err_t xiaozhi_recorder_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 帧缓冲池：QVGA JPEG 一帧 12~30KB，池子放 PSRAM，绝不占内部 RAM */
    int pool_ok = 0;
    for (int i = 0; i < XIAOZHI_REC_POOL_FRAMES; i++) {
        s_pool[i].buf = (uint8_t *)heap_caps_malloc(XIAOZHI_REC_FRAME_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_pool[i].cap = (s_pool[i].buf != NULL) ? XIAOZHI_REC_FRAME_MAX_BYTES : 0;
        s_pool[i].busy = false;
        if (s_pool[i].buf != NULL) {
            pool_ok++;
        }
    }
    if (pool_ok == 0) {
        ESP_LOGE(TAG, "no psram for frame pool, recorder disabled");
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(XIAOZHI_REC_QUEUE_LEN, sizeof(rec_frame_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "create queue failed");
        return ESP_ERR_NO_MEM;
    }

    /* 栈放 PSRAM（xiaozhi_task_create 统一处理，失败自动回落内部 RAM）。
     * 本任务只做 SD 卡 FATFS 读写，不调用会禁用 cache 的 flash 操作，PSRAM 栈安全。 */
    if (xiaozhi_task_create(rec_task, "recorder", XIAOZHI_STACK_6K, NULL, XIAOZHI_PRIO_LOW, &s_task,
                            XIAOZHI_REC_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "create task failed");
        return ESP_ERR_NO_MEM;
    }

    xiaozhi_mode_ops_t ops = {
        .on_enter = rec_mode_monitor_enter,
        .on_exit = rec_mode_monitor_exit,
        .on_trigger = NULL,
        .ctx = NULL,
    };
    esp_err_t err = xiaozhi_appmode_register(XIAOZHI_MODE_MONITOR, &ops);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register monitor hooks failed: %s", esp_err_to_name(err));
        return err;
    }

#if XIAOZHI_REC_EVENT_ENABLE
    xiaozhi_doorbell_recorder_t backend = {
        .start = rec_doorbell_start,
        .stop = rec_doorbell_stop,
        .ctx = NULL,
    };
    (void)xiaozhi_doorbell_set_recorder(&backend);
#endif

    s_inited = true;
    ESP_LOGI(TAG, "init ok: %d/%d frame buffers, continuous=%d, event=%d, slice=%u ms, keep-free=%llu MB",
             pool_ok, XIAOZHI_REC_POOL_FRAMES, XIAOZHI_REC_MONITOR_CONTINUOUS, XIAOZHI_REC_EVENT_ENABLE,
             (unsigned)XIAOZHI_REC_SLICE_MS, (unsigned long long)(XIAOZHI_REC_KEEP_FREE_BYTES / (1024 * 1024)));
    return ESP_OK;
}

esp_err_t xiaozhi_recorder_start_continuous(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!xiaozhi_camera_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!xiaozhi_camview_is_open()) {
        /* 没有预览任务就没有帧来源；监控模式钩子的执行顺序保证 preview 先开 */
        ESP_LOGW(TAG, "preview not open, skip continuous record");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_want && !s_event) {
        return ESP_OK; /* 已在连续录像 */
    }
    if (s_want) {
        xiaozhi_recorder_stop();
    }

    strncpy(s_prefix, "REC", sizeof(s_prefix) - 1);
    s_prefix[sizeof(s_prefix) - 1] = '\0';
    s_base_from_id = false; /* 连续录像要自己拼时间戳：REC_<时间>.avi */
    s_event = false;
    s_deadline_us = 0;
    s_slice_ms = XIAOZHI_REC_SLICE_MS;
    s_cam_owned = false;
    s_want = true;
    s_retry_at_us = 0;
    s_dropped = 0;
    s_session_files = 0;
    s_session_bytes = 0;
    s_fps = 0.0f;

    if (!s_tap_registered) {
        esp_err_t err = xiaozhi_camview_set_frame_tap(XIAOZHI_CAMVIEW_TAP_RECORDER, rec_on_frame, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "attach frame tap failed: %s", esp_err_to_name(err));
        } else {
            s_tap_registered = true;
        }
    }

    ESP_LOGI(TAG, "continuous record start (slice=%u ms)", (unsigned)s_slice_ms);
    return ESP_OK;
}

esp_err_t xiaozhi_recorder_start_event(const char *event_id, uint32_t duration_ms)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_want) {
        xiaozhi_recorder_stop();
    }

    if (duration_ms == 0) {
        duration_ms = XIAOZHI_REC_SLICE_MS;
    }

    strncpy(s_prefix, (event_id != NULL && event_id[0] != '\0') ? event_id : "EVT", sizeof(s_prefix) - 1);
    s_prefix[sizeof(s_prefix) - 1] = '\0';
    /* 门铃 event_id 已含时间（EVT_<YYYYMMDD_HHMMSS>），直接作为文件名主体 */
    s_base_from_id = true;

    s_event = true;
    s_slice_ms = duration_ms; /* 事件录像不切片，录完即止 */
    s_deadline_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    s_dropped = 0;
    s_session_files = 0;
    s_session_bytes = 0;
    s_fps = 0.0f;

    if (!xiaozhi_camview_is_open()) {
        /* 门铃模式没有预览：由本模块独占摄像头（grab 由录像任务自己做），
         * 录完在 rec_release_resources() 里 deinit 归还 PSRAM */
        if (!xiaozhi_camera_ready()) {
            esp_err_t err = xiaozhi_camera_init(XIAOZHI_CAM_FMT_JPEG);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "camera init for event record failed: %s", esp_err_to_name(err));
                s_event = false;
                s_deadline_us = 0;
                return err;
            }
        }
        s_cam_owned = true;
        ESP_LOGI(TAG, "event record owns camera");
    } else if (!s_tap_registered) {
        esp_err_t err = xiaozhi_camview_set_frame_tap(XIAOZHI_CAMVIEW_TAP_RECORDER, rec_on_frame, NULL);
        if (err == ESP_OK) {
            s_tap_registered = true;
        }
    }

    s_want = true;
    s_retry_at_us = 0;
    s_last_write_us = 0;

    ESP_LOGI(TAG, "event record start: prefix=%s duration=%u ms", s_prefix, (unsigned)duration_ms);
    return ESP_OK;
}

void xiaozhi_recorder_stop(void)
{
    if (!s_inited) {
        return;
    }
    if (!s_want && !s_file_open) {
        rec_release_resources();
        return;
    }

    s_want = false;

    /* 等 worker 关文件（最长 2s）；正常情况 50~150ms 内完成 */
    for (int i = 0; i < 100 && s_file_open; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_file_open) {
        ESP_LOGW(TAG, "stop timeout: file still open, worker will close it later");
    }

    rec_release_resources();

    ESP_LOGI(TAG, "record stop: %u files, %llu KB, %u dropped, last fps %.1f", (unsigned)s_session_files,
             (unsigned long long)(s_session_bytes / 1024), (unsigned)s_dropped, s_fps);
    s_state = XIAOZHI_REC_STATE_IDLE;
    s_session_files = 0;
    s_session_bytes = 0;
}

bool xiaozhi_recorder_is_active(void)
{
    return s_want;
}

void xiaozhi_recorder_get_status(xiaozhi_rec_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->active = s_want;
    out->event = s_event;
    out->file_open = s_file_open;
    out->frames = s_avi.frames;
    out->files = s_session_files;
    out->bytes = s_session_bytes;
    out->dropped = s_dropped;
    out->fps = s_fps;
    strncpy(out->file, s_file_rel, sizeof(out->file) - 1);
    strncpy(out->reason, s_reason, sizeof(out->reason) - 1);
}

#else /* XIAOZHI_REC_ENABLE == 0：空实现，不建任务、不占 PSRAM、不写 SD */

esp_err_t xiaozhi_recorder_init(void)
{
    return ESP_OK;
}
esp_err_t xiaozhi_recorder_start_continuous(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t xiaozhi_recorder_start_event(const char *event_id, uint32_t duration_ms)
{
    (void)event_id;
    (void)duration_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
void xiaozhi_recorder_stop(void)
{
}
bool xiaozhi_recorder_is_active(void)
{
    return false;
}
void xiaozhi_recorder_get_status(xiaozhi_rec_status_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->state = XIAOZHI_REC_STATE_IDLE;
    }
}
const char *xiaozhi_recorder_state_name(xiaozhi_rec_state_t state)
{
    (void)state;
    return "disabled";
}

#endif /* XIAOZHI_REC_ENABLE */
