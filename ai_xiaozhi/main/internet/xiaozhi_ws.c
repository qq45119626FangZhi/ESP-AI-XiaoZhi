#include "xiaozhi_ws.h"
#include <stdio.h>

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

esp_websocket_client_handle_t ws_client;

extern char mac_str[18];
extern char uuid_str[37];

static char *iot_desc_json_str =
#include "iot_desc.json"
;
    

static char *iot_state_json_str =
#include "iot_init_status.json"
;
    

#define TAG "xiaozhi_ws"
void xiaozhi_ws_init(void)
{
    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = xiaozhi_handle.websocket_url;
    // 设置请求的方式是wss协议
    websocket_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
    // 设置证书验证的方式
    websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    websocket_cfg.reconnect_timeout_ms = 3000;
    websocket_cfg.network_timeout_ms = 3000;
    // websocket client init
    ws_client = esp_websocket_client_init(&websocket_cfg);

    // 设置请求头
    // Bearer test-token
    size_t authorization_len = strlen("Bearer ") + strlen(xiaozhi_handle.websocket_token) + 1;
    char *authorization_str = heap_caps_malloc(authorization_len, MALLOC_CAP_SPIRAM);
    sprintf(authorization_str, "Bearer %s", xiaozhi_handle.websocket_token);
    esp_websocket_client_append_header(ws_client, "Authorization", authorization_str);
    esp_websocket_client_append_header(ws_client, "Protocol-Version", "1");
    esp_websocket_client_append_header(ws_client, "Device-Id", mac_str);
    esp_websocket_client_append_header(ws_client, "Client-Id", uuid_str);

    // 注册回调
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    // 发请求，建立连接
    // esp_websocket_client_start(ws_client);
    // vTaskDelay(5000 / portTICK_PERIOD_MS);
    // xiaozhi_ws_send_hello();
    // vTaskDelay(5000 / portTICK_PERIOD_MS);
    // xiaozhi_ws_send_wkup_word();
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_BEGIN:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_BEGIN");
            break;
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            // ! 主动修改连接成功的标志位
            xEventGroupSetBits(xiaozhi_handle.ws_event_group, WS_CONNECTED_BIT);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_IDLE;
            xiaozhi_handle.is_wakeup = false;
            break;
        case WEBSOCKET_EVENT_DATA:
            // ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
            // ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
            // 判断是文本帧数据
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                ESP_LOGI(TAG, "Received text data, data: %.*s", data->data_len, data->data_ptr);
                if (xiaozhi_handle.process_text_frame != NULL) {
                    xiaozhi_handle.process_text_frame(data->data_ptr);
                }
            }
            // 判断是二进制数据帧数据
            if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                //ESP_LOGI(TAG, "Received bin data len=%d", data->data_len);
                if (xiaozhi_handle.process_binary_audio_frame != NULL) {
                    xiaozhi_handle.process_binary_audio_frame(data->data_ptr, data->data_len);
                }
            }
            if (data->op_code == WS_TRANSPORT_OPCODES_CLOSE) {
                ESP_LOGI(TAG, "Received bin data len=%d", data->data_len);
                xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_IDLE;
                xiaozhi_handle.is_wakeup = false;
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
            xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_IDLE;
            xiaozhi_handle.is_wakeup = false;
            break;
        case WEBSOCKET_EVENT_FINISH:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_FINISH");
            xiaozhi_handle.xiaozhi_state = XIAOZHI_STATE_IDLE;
            xiaozhi_handle.is_wakeup = false;
            break;
    }
}

/* 封装发送文本帧的方法 */
void xiaozhi_ws_send_text(char *data, size_t len) { esp_websocket_client_send_text(ws_client, data, len, portMAX_DELAY); }
/* 封装发送二进制数据帧的方法 */
void xiaozhi_ws_send_bin(char *data, size_t len) { esp_websocket_client_send_bin(ws_client, data, len, portMAX_DELAY); }

/* 封装发送hello消息 */
void xiaozhi_ws_send_hello(void)
{
    char *hello_str = "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"features\":{\"mcp\":true},\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                      "\"channels\":1,\"frame_duration\":60}}";
    xiaozhi_ws_send_text(hello_str, strlen(hello_str));
}

/* 封装发送唤醒词消息 */
void xiaozhi_ws_send_wkup_word(void)
{
    char *wkup_str = "{\"type\":\"listen\",\"state\":\"detect\",\"text\":\"你好,小智\"}";
    xiaozhi_ws_send_text(wkup_str, strlen(wkup_str));
}

/* 封装将建立连接的方法异步转同步 */
bool xiaozhi_ws_connect_sync(void)
{
    // 判断是否已经初始化了客户端
    if (esp_websocket_client_is_connected(ws_client) == true) {
        return true;
    }
    // 发请求，建立连接
    esp_websocket_client_start(ws_client);
    // 以阻塞的形式等待，客户端和服务器端建立连接成功
    EventBits_t res = xEventGroupWaitBits(xiaozhi_handle.ws_event_group, WS_CONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    // 判断是否是超时
    if (!(res & WS_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "websocket connect timeout");
        return false;
    }

    return true;
}

/* 发送开始监听的功能函数 */
void xiaozhi_ws_send_start_listening(void)
{
    // char *start_listening = "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}";
    char *start_listening = "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}";
    xiaozhi_ws_send_text(start_listening, strlen(start_listening));
}

/* 发送停止监听的功能函数 */
void xiaozhi_ws_send_stop_listening(void)
{
    char *stop_listening = "{\"type\":\"listen\",\"state\":\"stop\"}";
    xiaozhi_ws_send_text(stop_listening, strlen(stop_listening));
}

/* 封装发送终止消息 */
void xiaozhi_ws_send_abort(void)
{
    char *abort = "{\"type\":\"abort\"}";
    xiaozhi_ws_send_text(abort, strlen(abort));
}

void xiaozhi_ws_send_iot_desc(void)
{
  ESP_LOGI(TAG, "send iot desc, len=%d", (int)strlen(iot_desc_json_str));
  xiaozhi_ws_send_text(iot_desc_json_str, strlen(iot_desc_json_str));
}

/* 发送远程控制初始化状态 */
void xiaozhi_ws_send_iot_state(void)
{
  xiaozhi_ws_send_text(iot_state_json_str, strlen(iot_state_json_str));
}

/* 上报当前工作模式（语音/门铃/监控），供云端与大模型同步 */
void xiaozhi_ws_send_iot_mode_state(const char *mode_name)
{
    if (ws_client == NULL || mode_name == NULL) {
        return;
    }
    // 未连接时直接跳过，避免 send_text 阻塞
    if (!esp_websocket_client_is_connected(ws_client)) {
        ESP_LOGI(TAG, "ws not connected, skip mode state: %s", mode_name);
        return;
    }

    char state_buf[160];
    int len = snprintf(state_buf, sizeof(state_buf),
                       "{\"type\":\"iot\",\"states\":[{\"name\":\"Mode\",\"state\":{\"mode\":\"%s\"}}],\"update\":true}",
                       mode_name);
    if (len <= 0 || len >= (int)sizeof(state_buf)) {
        ESP_LOGW(TAG, "mode state buffer too small");
        return;
    }
    xiaozhi_ws_send_text(state_buf, (size_t)len);
}

/* 上报门铃事件（SW2 长按触发），供云端/App 侧提示 */
void xiaozhi_ws_send_doorbell_event(const char *event_id, bool recording)
{
    if (ws_client == NULL || event_id == NULL) {
        return;
    }
    // 未连接时直接跳过，避免 send_text 阻塞
    if (!esp_websocket_client_is_connected(ws_client)) {
        ESP_LOGI(TAG, "ws not connected, skip doorbell event: %s", event_id);
        return;
    }

    char state_buf[192];
    int len = snprintf(state_buf, sizeof(state_buf),
                       "{\"type\":\"iot\",\"states\":[{\"name\":\"Doorbell\",\"state\":{\"event\":\"pressed\","
                       "\"id\":\"%s\",\"recording\":%s}}],\"update\":true}",
                       event_id, recording ? "true" : "false");
    if (len <= 0 || len >= (int)sizeof(state_buf)) {
        ESP_LOGW(TAG, "doorbell state buffer too small");
        return;
    }
    xiaozhi_ws_send_text(state_buf, (size_t)len);
}
