#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ==========================================================================
 * M5 监控录像（MJPEG-AVI）
 *   1. 封装：把摄像头输出的 JPEG 帧直接写进 AVI 容器（四字符码 MJPG / 流 00dc），
 *      PC/手机通用播放器（VLC / PotPlayer / ffmpeg）可直接播放，不需要转码。
 *      文件头部先写占位，运行期每秒回填一次（RIFF/movi 长度、帧数、宽高），
 *      关闭时再补 idx1 索引 —— 即使掉电，最后一个切片也只是缺索引，不会整片报废。
 *   2. 切片：单个文件最长 XIAOZHI_REC_SLICE_MS（默认 3 分钟，与需求一致），
 *      到点关闭当前文件再开新文件，避免 FAT 大文件写坏后全丢。
 *   3. 循环覆盖：开新片前检查剩余空间，低于保留水位就调 xiaozhi_sdcard_reclaim()
 *      从 DCIM 里删最旧的文件（只动 DCIM，不碰 LOG/CONFIG）。
 *   4. 卡满保护：回收后仍低于 XIAOZHI_REC_MIN_FREE_BYTES 时暂停写盘（状态 NO_SPACE），
 *      每 XIAOZHI_REC_RETRY_MS 重试，期间画面/语音完全不受影响。
 *   5. 异常掉盘降级：任何 fopen/fwrite/fsync 失败都立即关文件 → 卸载 SD
 *      （让 sd_monitor 任务走"拔卡重挂载"流程）→ 状态 NO_SD，卡回来后自动续录。
 *
 * 取帧方式（关键：摄像头只有一个消费者）
 *   - 监控模式：xiaozhi_camview 的预览任务已经每帧 grab，本模块通过
 *     xiaozhi_camview_set_frame_tap() 订阅同一帧（零额外取帧，预览帧率不掉）；
 *   - 事件录像（门铃）：那时没有预览，本模块自己 xiaozhi_camera_init() + grab，
 *     录完立即 deinit 归还 PSRAM（s_cam_owned）。
 *
 * 目录与命名（与 xiaozhi_sdcard.h 的约定一致）
 *   /sdcard/DCIM/<YYYY-MM-DD>/REC_YYYYMMDD_HHMMSS.avi     连续录像
 *   /sdcard/DCIM/<YYYY-MM-DD>/EVT_YYYYMMDD_HHMMSS.avi     门铃事件录像（沿用事件 ID）
 *   未授时（SNTP 未成功）落 /sdcard/DCIM/BOOT/REC_BOOT_<开机秒>.avi
 *
 * 全部实现由 XIAOZHI_REC_ENABLE 控制，=0 时为空实现（不建任务、不申请缓冲），
 * 不影响既有语音/预览/门铃链路。
 * ========================================================================== */

/* ---------------- 编译期开关 ---------------- */

/* 录像总开关：0 = 全部接口空实现（恢复"监控只做 UI 展示、不写 SD"的旧行为） */
#define XIAOZHI_REC_ENABLE              1

/* 1 = 进入监控模式即连续录像（S3 连续策略，需求"连续录 2 小时"）；
 * 0 = 监控模式只预览不录像（等价于 M3 已验收的行为） */
#define XIAOZHI_REC_MONITOR_CONTINUOUS  1

/* 1 = 向门铃模块注入录像后端（门铃事件录 XIAOZHI_DOORBELL_RECORD_MS 秒） */
#define XIAOZHI_REC_EVENT_ENABLE        1

/* 录像帧率：摄像头预览 10fps，多的帧在 tap 里直接丢，不写盘 */
#define XIAOZHI_REC_FPS                 8

/* 单个文件最长时长（ms）：需求规定单文件最长 3 分钟 */
#define XIAOZHI_REC_SLICE_MS            180000

/* 回填 AVI 头 + fsync 的周期（ms）：掉电最多丢这么多，文件仍可播 */
#define XIAOZHI_REC_SYNC_MS             1000

/* 剩余空间巡检周期（ms） */
#define XIAOZHI_REC_SPACE_CHECK_MS      5000

/* 循环覆盖保留水位（字节）：剩余空间低于它就开始删最旧的录像（大卡按此值，
 * 小卡会自动缩到容量的 1/4，见 xiaozhi_recorder.c 的 rec_keep_free_target()） */
#define XIAOZHI_REC_KEEP_FREE_BYTES     (500ULL * 1024 * 1024)

/* 硬保护：剩余空间低于该值就不开新文件（先回收，回收后仍不够则暂停录像） */
#define XIAOZHI_REC_MIN_FREE_BYTES      (16ULL * 1024 * 1024)

/* 拒绝/降级后的重试周期（ms） */
#define XIAOZHI_REC_RETRY_MS            5000

/* 帧缓冲池个数（PSRAM）：tap 拷进池子、worker 写盘，池子满了丢帧不阻塞预览 */
#define XIAOZHI_REC_POOL_FRAMES         4

/* 单帧上限（字节）：QVGA JPEG q=12 约 12~30KB，留足余量；超过直接丢帧 */
#define XIAOZHI_REC_FRAME_MAX_BYTES     (64 * 1024)

/* 待写队列深度 */
#define XIAOZHI_REC_QUEUE_LEN           4

/* AVI 头的默认宽高（真值由首帧回填，摄像头固定 QVGA） */
#define XIAOZHI_REC_W                   320
#define XIAOZHI_REC_H                   240

/* 录像任务：core0（栈与优先级统一见 xiaozhi_common.h，栈由 xiaozhi_task_create 放 PSRAM） */
#define XIAOZHI_REC_TASK_CORE           0

/* ---------------- 对外 API ---------------- */

/* 录像状态（供日志/网页状态页显示） */
typedef enum {
    XIAOZHI_REC_STATE_IDLE = 0, /* 未录像 */
    XIAOZHI_REC_STATE_RECORDING,/* 正在写文件 */
    XIAOZHI_REC_STATE_NO_SD,    /* SD 未挂载 / 写失败，等待重挂载 */
    XIAOZHI_REC_STATE_NO_SPACE, /* 回收后空间仍不足，暂停写盘 */
} xiaozhi_rec_state_t;

typedef struct {
    xiaozhi_rec_state_t state;
    bool active;    /* 是否处于"要求录像"状态（含等待 SD/空间） */
    bool event;     /* true = 事件录像（到时自动停），false = 连续录像 */
    bool file_open; /* 当前是否有文件正在写 */
    uint32_t frames;  /* 当前文件已写帧数 */
    uint32_t files;   /* 本次会话切出的文件数 */
    uint64_t bytes;   /* 本次会话累计写入的 JPEG 字节 */
    uint32_t dropped; /* 因池满/超时/超长被丢弃的帧数 */
    float fps;        /* 最近统计窗口的实测帧率 */
    char file[80];    /* 当前文件（相对 /sdcard，空串表示无） */
    char reason[48];  /* 最近一次降级/暂停原因 */
} xiaozhi_rec_status_t;

/* 初始化：申请帧缓冲池、创建队列与录像任务、注册监控模式钩子、
 * 并向门铃模块注入事件录像后端。须在 xiaozhi_camview_init() /
 * xiaozhi_doorbell_init() 之后调用一次；重复调用幂等。 */
esp_err_t xiaozhi_recorder_init(void);

/* 连续录像（监控模式）：一直录到 xiaozhi_recorder_stop()，内部自动切片与循环覆盖。
 * 需要先有摄像头（xiaozhi_camera_ready()），否则返回 ESP_ERR_INVALID_STATE。 */
esp_err_t xiaozhi_recorder_start_continuous(void);

/* 事件录像：录 duration_ms 毫秒后自动停止（0 表示按 XIAOZHI_REC_SLICE_MS）。
 * event_id 作为文件名前缀（门铃传 "EVT_YYYYMMDD_HHMMSS"）；摄像头没起来时
 * 本模块自行初始化并在录完后释放。 */
esp_err_t xiaozhi_recorder_start_event(const char *event_id, uint32_t duration_ms);

/* 停止录像：置停标志后等文件收尾（最长 2s），可重复调用 */
void xiaozhi_recorder_stop(void);

/* 是否正在录像（含等待 SD 恢复） */
bool xiaozhi_recorder_is_active(void);

/* 状态与统计（out 可为 NULL） */
void xiaozhi_recorder_get_status(xiaozhi_rec_status_t *out);

/* 状态名字符串（"idle"/"recording"/"no_sd"/"no_space"），供日志/网页使用 */
const char *xiaozhi_recorder_state_name(xiaozhi_rec_state_t state);
