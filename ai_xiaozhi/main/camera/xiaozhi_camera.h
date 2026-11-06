#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

/* ==========================================================================
 * OV2640 摄像头（M2）
 *   1. DVP 12 脚初始化（引脚见 xiaozhi_camera_pins.h，SCCB 复用音频的 I2C 总线）
 *   2. 双像素格式：QVGA(320x240) JPEG（抓拍/录像）与 RGB565（直出画面）
 *      - JPEG 为默认格式：抓拍直接写文件，预览由 esp_jpeg 解码缩放
 *      - RGB565 更省 CPU，但无法直接落盘 JPEG
 *   3. xiaozhi_camera_snapshot() 把一帧 JPEG 写到 SD：
 *        /sdcard/DCIM/<YYYY-MM-DD>/SNAP_YYYYMMDD_HHMMSS.jpg （已授时）
 *        /sdcard/DCIM/BOOT/SNAP_BOOT_<开机秒>_<序号>.jpg    （未授时，绝不写 1970）
 *   4. 全部实现由 XIAOZHI_CAM_ENABLE 开关控制，=0 时退化为空实现
 *
 * 依赖与顺序（重要）：
 *   SCCB 复用 xiaozhi_audio_init() 创建的 I2C 总线，因此
 *   **必须在音频初始化之后**调用 xiaozhi_camera_init()。
 *   摄像头出图（DVP/JPEG）不占用 SCCB，运行期可自由抓帧；
 *   但修改传感器参数（切换像素格式/分辨率/亮度）需要 SCCB，同样只能在音频就绪后。
 * ========================================================================== */

/* ---------------- 编译期开关 ---------------- */

/* 摄像头总开关：0 = 全部接口为空实现（不初始化 DVP，不占用 PSRAM） */
#define XIAOZHI_CAM_ENABLE           1

/* 上电即初始化摄像头（M2 原有行为）：
 *   1 = 上电 xiaozhi_camera_init() + xiaozhi_camera_boot_snapshots() 抓拍自检；
 *   0 = 上电不启用摄像头（默认）：只有切到监控模式时由 xiaozhi_camview 的模式钩子
 *       xiaozhi_camera_init()，退出监控模式立即 deinit 归还 PSRAM。 */
#define XIAOZHI_CAM_BOOT_INIT        0

/* 摄像头保持初始化（默认 1，2026-11-06 修复"二次切换监控模式失败"）：
 * 本板 OV2640 的 PWDN/RESET 均为 -1（硬件固定，无 GPIO 控制），
 * esp_camera_deinit() 会停掉 XCLK，之后 esp_camera_init() 重新探测 SCCB 会一直
 *   I2C hardware timeout / probe device timeout -> Detected camera not supported
 * （实机日志，只有整机复位才能恢复，所以表现为"复位后第一次能切、第二次失败"）。
 * 因此退出监控模式 / 门铃事件录像结束后**不再 deinit 摄像头**，只关闭预览视图；
 * 摄像头仅在首次进入监控模式时初始化一次，之后反复切换都复用（省掉 SCCB 重探测）。
 * 置 0 恢复旧行为（每次退出都 deinit，本板会无法再次进入监控模式）。 */
#define XIAOZHI_CAM_KEEP_INIT        1

/* XCLK 频率（OV2640 支持 6~27MHz，20MHz 为常用值） */
#define XIAOZHI_CAM_XCLK_HZ          (20 * 1000 * 1000)

/* 帧缓冲个数（都放 PSRAM，QVGA JPEG 每槽约 20~40KB）
 *
 * 为什么是 3 而不是 2（2026-11-06 实测定位）：
 *   cam_hal 的 cam_task 每开一帧都要找一个 en==1（空闲）的槽（cam_get_next_frame）。
 *   应用层（预览任务）拿到的帧在解码+缩放+LVGL 刷新期间一直占着一个槽，
 *   另一个槽如果已经排在 fb 队列里等着被取，两个槽就全被占用，
 *   cam_start_frame() 失败 -> cam_task 退回 CAM_STATE_IDLE -> VSYNC 事件在中断里
 *   塞不进队列（ll_cam_send_event 里 ll_cam_stop + 打印 "EV-VSYNC-OVF"）-> 采集停摆。
 *   症状：串口刷 "cam_hal: EV-VSYNC-OVF - 100 additional misses"、
 *         "Failed to get frame: timeout"、预览任务一直 grab failed -> 监控模式全黑。
 *   3 个槽让"采集中 / 排队中 / 应用处理中"同时存在，不会互相卡死。 */
#define XIAOZHI_CAM_FB_COUNT         3

/* 相机 DMA 缓冲放在哪里（决定性开关，内部 SRAM 紧张时直接决定能否初始化成功）：
 *   0 = 自动（推荐）：先看内部 RAM 是否够（16KB 连续 DMA 缓冲 + 4KB cam_task 栈），
 *       够则走内部 DMA（官方默认行为，画质/带宽最好），不够则自动改用 PSRAM DMA；
 *   1 = 强制 PSRAM DMA：ESP32-S3/S2 支持，DVP DMA 直接写 PSRAM 帧缓冲，
 *       不再申请 16KB 连续内部 RAM；
 *   2 = 强制内部 DMA：官方默认，需要 16KB 连续内部 RAM + 4KB 任务栈。
 *
 * 背景：监控模式是在语音链路（Wi-Fi/TLS/esp-sr AFE/LVGL）全部起来之后才初始化相机，
 * 那时内部 RAM 常常只剩几 KB，于是报
 *   cam_dma_config: DMA buffer 16384 Byte malloc failed, the current largest free block:xxxx Byte
 * 自动模式会在这个场景下切到 PSRAM DMA（初始化失败时还会反向重试一次）。 */
/* 2026-11-06 改为 1（强制 PSRAM DMA），原因：
 *   语音对话 + 监控模式共存时，内部 RAM 已经很紧。自动模式在"内部最大连续块 >= 24KB"
 *   时会选内部 DMA，由 cam_dma_config() 一次性 heap_caps_malloc 16KB MALLOC_CAP_DMA
 *   连续块（cam_hal.c:522）。这块正好是 mbedtls 硬件 AES-DMA 想拿的内存，
 *   结果实测出现 "esp-aes: Failed to allocate memory" -> TLS 写失败 -> WebSocket 断开
 *   -> 小智无法唤醒，同时摄像头自己也刷 "cam_hal: EV-VSYNC-OVF"。
 *   强制 PSRAM DMA 后不再申请这块 16KB 内部缓冲（帧缓冲本来就在 PSRAM），
 *   把连续内部 RAM 留给 AES-DMA / LVGL / httpd。 */
#define XIAOZHI_CAM_DMA_MODE         1

/* 自动模式的门限：内部最大连续空闲块 >= 该值才认为"内部 DMA 够用"
 * （16KB DMA 缓冲 + 4KB cam_task 栈 + 4KB 余量） */
#define XIAOZHI_CAM_INTERNAL_DMA_MIN (24 * 1024)

/* JPEG 压缩质量：0~63，数值越小画质越好、体积越大 */
#define XIAOZHI_CAM_JPEG_QUALITY     12

/* 上电抓拍张数（M2 验收项：上电抓 3 张 QVGA JPEG 到 SD） */
#define XIAOZHI_CAM_BOOT_SNAPSHOTS   3

/* 抓拍根目录（DCIM 由 xiaozhi_sdcard_init() 创建） */
#define XIAOZHI_CAM_SNAPSHOT_DIR     "/sdcard/DCIM"

/* ---------------- 对外 API ---------------- */

typedef enum {
    XIAOZHI_CAM_FMT_JPEG = 0, /* QVGA JPEG：抓拍/录像默认格式 */
    XIAOZHI_CAM_FMT_RGB565,   /* QVGA RGB565：直出，省解码 CPU */
} xiaozhi_cam_format_t;

/* 初始化（幂等）：app_main 中必须在 xiaozhi_audio_init() 之后调用 */
esp_err_t xiaozhi_camera_init(xiaozhi_cam_format_t fmt);

/* 运行期切换像素格式（内部 deinit + 重新 init，约 200ms，会短暂中断预览） */
esp_err_t xiaozhi_camera_set_format(xiaozhi_cam_format_t fmt);

/* 反初始化：释放帧缓冲、停止 XCLK（不删除共享 I2C 总线，音频不受影响） */
esp_err_t xiaozhi_camera_deinit(void);

/* 是否已就绪 */
bool xiaozhi_camera_ready(void);

/* 当前像素格式 */
xiaozhi_cam_format_t xiaozhi_camera_format(void);

/* 取一帧（阻塞直到有帧；未初始化返回 NULL）。用完必须 xiaozhi_camera_return() */
camera_fb_t *xiaozhi_camera_grab(void);

/* 归还帧缓冲（必须与 grab 成对） */
void xiaozhi_camera_return(camera_fb_t *fb);

/* 抓拍一张 JPEG 存 SD；tag 为文件名前缀（NULL 时用 "SNAP"）。
 * SD 未挂载或写失败时返回错误，不影响其它功能 */
esp_err_t xiaozhi_camera_snapshot(const char *tag);

/* 上电自检抓拍：连续抓 count 张 JPEG（间隔 200ms），返回成功张数 */
int xiaozhi_camera_boot_snapshots(int count);
