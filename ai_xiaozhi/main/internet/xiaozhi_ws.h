#pragma once
#include <stdbool.h>

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "xiaozhi_common.h"
#include "string.h"
#include "esp_heap_caps.h"

void xiaozhi_ws_init(void);

/* 封装发送hello消息 */
void xiaozhi_ws_send_hello(void);
/* 封装发送二进制数据帧的方法 */
void xiaozhi_ws_send_bin(char *data, size_t len);

/* 封装发送唤醒词消息 */
void xiaozhi_ws_send_wkup_word(void);

/* 封装将建立连接的方法异步转同步 */
bool xiaozhi_ws_connect_sync(void);

/* 发送开始监听的功能函数 */
void xiaozhi_ws_send_start_listening(void);
/* 发送停止监听的功能函数 */
void xiaozhi_ws_send_stop_listening(void);
/* 封装发送终止消息 */
void xiaozhi_ws_send_abort(void);

void xiaozhi_ws_send_iot_desc(void);

/* 发送远程控制初始化状态 */
void xiaozhi_ws_send_iot_state(void);

/* 上报当前工作模式（语音/门铃/监控），供云端与大模型同步 */
void xiaozhi_ws_send_iot_mode_state(const char *mode_name);

/* 上报门铃事件（SW2 长按），供云端/App 侧提示；ws 未连接时内部直接跳过 */
void xiaozhi_ws_send_doorbell_event(const char *event_id, bool recording);
