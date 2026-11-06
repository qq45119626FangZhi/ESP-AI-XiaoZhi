#pragma once
#include "esp_log.h"
#include "xiaozhi_lcd.h"
#include "esp_lvgl_port.h"
#include "font_emoji.h"

void xiaozhi_lvgl_init(void);

/* 显示hello world */
void xiaozhi_lvgl_show_hello_world(void);

/* 显示二维码 */
void xiaozhi_lvgl_show_qrcode(char *qrcode_data);

/* 删除二维码 */
void xiaozhi_lvgl_delete_qrcode(void);

/* 修改标题的方法 */
void xiaozhi_lvgl_set_title(char *title);
/* 修改文本的方法 */
void xiaozhi_lvgl_set_text(char *text);
/* 修改emoji的方法 */
void xiaozhi_lvgl_set_emoji(char *emoji_name);

/* 修改标题栏右侧的北京时间（"YYYY-MM-DD HH:MM:SS"），由 xiaozhi_time 每秒调用 */
void xiaozhi_lvgl_set_datetime(const char *datetime);

/* 标题栏最右侧的 WiFi 状态标识（语音/门铃模式可见，监控模式全屏预览时不显示） */
typedef enum {
    XIAOZHI_WIFI_STATE_CONNECTING = 0, /* 连接中（上电默认，灰色） */
    XIAOZHI_WIFI_STATE_CONNECTED,      /* 连接成功（拿到 IP，绿色） */
    XIAOZHI_WIFI_STATE_FAILED,         /* 连接失败/断开（红色） */
} xiaozhi_wifi_state_t;

/* 更新 WiFi 状态标识；界面未就绪时只缓存状态，界面创建后自动补显示 */
void xiaozhi_lvgl_set_wifi_state(xiaozhi_wifi_state_t state);