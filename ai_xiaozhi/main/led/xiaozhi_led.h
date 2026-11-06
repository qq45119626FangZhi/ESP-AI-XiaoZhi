#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ==========================================================================
 * WS2812 状态灯（LED2 / LED3 级联，DIN = GPIO46）
 *   1. 硬件：两颗 WS2812 级联，MCU 只驱动 DIN（GPIO46），第二颗由第一颗 DOUT 转发；
 *      数据顺序 GRB、每字节高位先出，单总线时序由 RMT 外设产生（不占用 CPU）。
 *   2. 索引：0 = LED2（靠近 DIN 的第一颗，默认用它做状态灯），1 = LED3。
 *      若实际点亮的不是 LED2，把 XIAOZHI_LED_CAMERA_INDEX 改成 1 即可。
 *   3. 本模块与业务解耦：只提供“设置某颗灯颜色”的 API；
 *      XIAOZHI_LED_FOLLOW_CAMVIEW=1 时由一个 200ms 轮询任务**只读**地观察
 *      xiaozhi_camview_is_open()（= 监控模式摄像头出图成功），
 *      打开则点亮 LED2 绿色、关闭（切到语音/门铃）则熄灭。
 *      这样不改动已验收的 xiaozhi_camview / xiaozhi_appmode 任何代码路径。
 *   4. 开关：XIAOZHI_LED_ENABLE=0 时全部退化为空实现，不申请 RMT 通道、不建任务。
 *   5. 注意：WS2812 满亮度电流较大，默认只给绿色约 1/3 亮度（XIAOZHI_LED_BRIGHT_G），
 *      实际观感不够亮时改该宏即可。
 * ========================================================================== */

/* ---------------- 编译期开关 ---------------- */

/* 状态灯总开关：0 = 全部接口空实现 */
#define XIAOZHI_LED_ENABLE          1

/* DIN 所在 GPIO（原理图网表：WS2812 DIN = GPIO46） */
#define XIAOZHI_LED_GPIO            46

/* 级联灯珠数量（LED2 + LED3） */
#define XIAOZHI_LED_COUNT           2

/* 监控模式状态灯使用的灯珠索引：0 = LED2，1 = LED3 */
#define XIAOZHI_LED_CAMERA_INDEX    0

/* 状态灯绿色亮度（0~255），WS2812 很亮，96 已足够醒目 */
#define XIAOZHI_LED_BRIGHT_G        96

/* 1 = 自动跟随摄像头画面状态（监控模式点亮 LED2 绿色，其它模式熄灭） */
#define XIAOZHI_LED_FOLLOW_CAMVIEW  1

/* 跟随轮询周期（ms）：模式切换后最长这么多毫秒点亮/熄灭 */
#define XIAOZHI_LED_POLL_MS         200

/* ---------------- 对外 API ---------------- */

/* 初始化（main.c 初始化区调用一次）：配置 RMT 通道 + 编码器，清空所有灯珠。
 * 可重复调用（幂等）。返回 ESP_OK 表示 RMT 通道已就绪。 */
esp_err_t xiaozhi_led_init(void);

/* 设置某颗灯珠颜色（index 从 0 开始，0 = LED2）。值为 0~255。
 * 颜色未变化时不重复发送（省总线时间）。 */
esp_err_t xiaozhi_led_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/* 设置所有灯珠为同一颜色 */
esp_err_t xiaozhi_led_set_all(uint8_t r, uint8_t g, uint8_t b);

/* 全部熄灭 */
void xiaozhi_led_clear(void);

/* 监控模式状态灯：on=true 点亮 LED2 绿色；on=false 熄灭 LED2（不影响其它灯珠）。
 * 通常由内部跟随任务调用，也可被其它业务显式调用。 */
void xiaozhi_led_set_camera(bool on);
