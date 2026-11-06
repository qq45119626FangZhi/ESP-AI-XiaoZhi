#pragma once

#include <stdint.h>

/* ==========================================================================
 * 画面时间水印（叠加在摄像头预览画面上的北京时间）
 *
 * 为什么不用 LVGL 标签做叠加：
 *   监控模式全屏预览时每帧都会 lv_obj_invalidate(整屏)，再让 LVGL 每秒多画一次
 *   标签会额外占用 panel io（历史上曾触发 tx_color 入队失败）。因此这里**直接把
 *   字符画进图像像素缓冲**（RGB565），跟着预览帧一起刷新，不新增任何 LVGL 重绘。
 *
 * 行为：
 *   - 时间来源 xiaozhi_time_format()（SNTP 校时后的 CST-8 北京时间）；
 *   - 未授时（RTC 还是 1970 / 校时未成功）**不画任何东西**，绝不显示假时间；
 *   - 只在监控全屏（320x240）画；右上角小叠加（80x100）放不下会自动跳过；
 *   - XIAOZHI_CAMSTAMP_ENABLE=0 时全部为空实现。
 * ========================================================================== */

/* 总开关 */
#define XIAOZHI_CAMSTAMP_ENABLE      1

/* 字号：点阵 5x7，scale=2 → 每字符 10x14，整串 "YYYY-MM-DD HH:MM:SS" 约 226x14 */
#define XIAOZHI_CAMSTAMP_SCALE       2

/* 字符间距（单位：像素 × scale） */
#define XIAOZHI_CAMSTAMP_GAP         1

/* 距画面边缘的留白（像素） */
#define XIAOZHI_CAMSTAMP_MARGIN_X    4
#define XIAOZHI_CAMSTAMP_MARGIN_Y    4

/* 文字颜色（RGB565，本机小端）：0xFFFF = 白色 */
#define XIAOZHI_CAMSTAMP_COLOR       0xFFFF

/* 1 = 文字底下压一层 50% 黑底，保证浅色画面上也看得清；0 = 只画字 */
#define XIAOZHI_CAMSTAMP_BOX         1

/* 水印位置 */
#define XIAOZHI_CAMSTAMP_POS_TOP_LEFT     0
#define XIAOZHI_CAMSTAMP_POS_TOP_RIGHT    1
#define XIAOZHI_CAMSTAMP_POS_BOTTOM_LEFT  2
#define XIAOZHI_CAMSTAMP_POS_BOTTOM_RIGHT 3
#define XIAOZHI_CAMSTAMP_POS         XIAOZHI_CAMSTAMP_POS_TOP_LEFT

/* 把当前北京时间“烧”进一张 RGB565 图像缓冲（buf 必须是 w*h 个 uint16，本机字节序）。
 * 由预览任务在缩放完成后、invalidate 之前调用；未授时直接返回、缓冲不变。 */
void xiaozhi_camstamp_draw(uint16_t *buf, int w, int h);
