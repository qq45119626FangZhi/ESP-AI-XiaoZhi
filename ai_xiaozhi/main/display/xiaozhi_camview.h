#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ==========================================================================
 * 摄像头预览视图（M2/M3）
 *   两种显示形态，共用同一个 LVGL 图像对象与预览任务：
 *     1. 右上角叠加（80x100，overlay）：叠在 AI 小智界面之上；
 *     2. 全屏（320x240，与 LCD 同尺寸）：监控模式使用。
 *
 *   画面来源：预览任务（core0 / prio3）周期性 xiaozhi_camera_grab() 取帧
 *     - JPEG 帧用 esp_jpeg 解码成 RGB565（PSRAM 缓冲）
 *     - RGB565 帧直接使用
 *   再按目标尺寸做最近邻缩放写入 LVGL 图像缓冲（PSRAM），持 LVGL 锁后 invalidate。
 *
 *   默认行为（XIAOZHI_CAMVIEW_MODE_BIND=1）：
 *     - 上电（语音模式）**不创建画面、不启用摄像头**；
 *     - 只有切到监控模式时由模式钩子启动摄像头 + 全屏预览；
 *     - 切换到其它模式时关闭画面并释放摄像头（deinit，归还 PSRAM）。
 *
 *   开关：XIAOZHI_CAMVIEW_ENABLE=0 时全部为空实现，不创建任务、不申请缓冲。
 * ========================================================================== */

/* 叠加预览总开关 */
#define XIAOZHI_CAMVIEW_ENABLE      1

/* 叠加画面尺寸（像素） */
#define XIAOZHI_CAMVIEW_W           80
#define XIAOZHI_CAMVIEW_H           100

/* 相对屏幕右上角的偏移（正值 = 向左/向下） */
#define XIAOZHI_CAMVIEW_OFFSET_X    0
#define XIAOZHI_CAMVIEW_OFFSET_Y    0

/* 叠加预览刷新率（帧/秒） */
#define XIAOZHI_CAMVIEW_FPS         8

/* 全屏预览尺寸（与 LCD 的 320x240 一致，监控模式使用） */
#define XIAOZHI_CAMVIEW_FULL_W      320
#define XIAOZHI_CAMVIEW_FULL_H      240

/* 全屏预览刷新率（帧/秒）：决策记录要求预览底线 10fps */
#define XIAOZHI_CAMVIEW_FULL_FPS    10

/* 缩放映射表最大宽度（取叠加/全屏两者较大值） */
#define XIAOZHI_CAMVIEW_MAX_W       320

/* JPEG 解码降采样倍数：1 = 原分辨率解码后再缩放（画质最好），2/4 = 交给硬件降采样（省 CPU） */
#define XIAOZHI_CAMVIEW_JPEG_SCALE_DIV 1

/* 摄像头给的是 RGB565（高字节在前），LVGL 需要本机小端，1 = 转换字节序 */
#define XIAOZHI_CAMVIEW_SWAP_SRC_BYTES 1

/* JPEG 解码输出的 RGB565 字节序：0 = 本机小端（默认），1 = 大端（高字节在前）
 *
 * 为什么必须是 0：
 *   1. LVGL 的内部图像数据按“本机字节序的 uint16”解释（ESP32-S3 是小端）；
 *   2. 屏幕要的是大端，这一步由 esp_lvgl_port 的 display flags.swap_bytes=true
 *      在 flush 时统一做 lv_draw_sw_rgb565_swap()（见 xiaozhi_lvgl.c）；
 *   3. 而 esp_jpeg 的 swap_color_bytes=1 输出的正好是大端（jpeg_decoder.c 里
 *      dst[0]=HIBYTE / dst[1]=LOBYTE），它适合直接喂 esp_lcd_panel_draw_bitmap
 *      （xiaozhi_lcd_show 的用法），若塞进 LVGL 图像就会被再交换一次
 *      → 画面轮廓还在但颜色全乱（典型“花屏/彩色噪点”，红蓝通道互换）。
 * 只有当把 xiaozhi_lvgl.c 的 swap_bytes 改成 false 时，这里才要改成 1。 */
#define XIAOZHI_CAMVIEW_JPEG_SWAP_BYTES 0

/* 按工作模式绑定（默认 1）：
 *   1 = 仅监控模式启动摄像头 + 全屏预览，其它模式无画面、摄像头不启用；
 *   0 = 旧行为：init 时直接开右上角叠加预览（需配合 XIAOZHI_CAM_BOOT_INIT=1） */
#define XIAOZHI_CAMVIEW_MODE_BIND   1

/* ---------------- 对外 API ---------------- */

/* 初始化：注册监控模式钩子（不创建画面、不启动任务、不启用摄像头）
 * 须在 xiaozhi_lvgl_init() 之后调用；XIAOZHI_CAMVIEW_MODE_BIND=0 时退化为直接开叠加预览
 * （该情况下须已调用 xiaozhi_camera_init()） */
esp_err_t xiaozhi_camview_init(void);

/* 打开画面并启动预览任务（fullscreen=true 全屏 / false 右上角叠加）。
 * 摄像头必须已就绪（xiaozhi_camera_ready()）；同尺寸重复调用是幂等的 */
esp_err_t xiaozhi_camview_open(bool fullscreen);

/* 关闭画面：停止预览任务 → 删除 LVGL 对象 → 释放 PSRAM 缓冲（不 deinit 摄像头） */
esp_err_t xiaozhi_camview_close(void);

/* 画面是否已打开 */
bool xiaozhi_camview_is_open(void);

/* 显示/隐藏画面（不影响摄像头本身出帧，也不启停预览任务） */
void xiaozhi_camview_set_visible(bool visible);
bool xiaozhi_camview_is_visible(void);

/* 关闭画面（= xiaozhi_camview_close，忽略返回值，保留旧接口名） */
void xiaozhi_camview_stop(void);

/* ---------------- 帧订阅（M5 录像 / M6 网页实时预览） ----------------
 *
 * 为什么需要它：摄像头同一时刻只能有一个"取帧者"，预览任务已经按
 * XIAOZHI_CAMVIEW_FULL_FPS 每帧 camera_grab()。录像和 HTTP 流如果各自再 grab，
 * 就会互相抢帧、把已验收的 10fps 预览拖到 5fps。
 * 这里让预览任务把**同一帧 JPEG** 在回调里分发出去（零额外取帧），
 * 订阅者只做内存拷贝，绝不能阻塞（回调在预览任务上下文同步执行）。
 *
 * 生命周期：只在监控模式（预览任务运行）期间有效；未注册任何订阅者时零开销。
 * 槽位固定 2 个，互不干扰：录像用 0、网页流用 1。 */
#define XIAOZHI_CAMVIEW_TAP_SLOTS    2
#define XIAOZHI_CAMVIEW_TAP_RECORDER 0 /* M5 录像 */
#define XIAOZHI_CAMVIEW_TAP_STREAM   1 /* M6 网页 MJPEG */

typedef void (*xiaozhi_camview_frame_cb_t)(const uint8_t *jpeg, size_t len, int w, int h, void *ctx);

/* 注册/注销订阅者（cb=NULL 表示注销该槽位）；线程安全，可在任意任务调用。
 * 仅在帧格式为 JPEG 时回调（RGB565 帧无法直接落盘/推流）。 */
esp_err_t xiaozhi_camview_set_frame_tap(int slot, xiaozhi_camview_frame_cb_t cb, void *ctx);
