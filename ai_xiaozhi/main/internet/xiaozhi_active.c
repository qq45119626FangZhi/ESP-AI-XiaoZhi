#include "xiaozhi_active.h"
#include "xiaozhi_appmode.h"

void gen_uuid(char *out);
esp_err_t _http_event_handler(esp_http_client_event_t *evt);
// 设置请求头
void xiaozhi_active_set_header(void);
// 设置请求体
void xiaozhi_active_set_body(void);
// 提取服务器返回的json数据
void xiaozhi_active_parse_json(char *json_data);

#define TAG "xiaozhi_active"

esp_http_client_handle_t client;

char mac_str[18];
char uuid_str[37];

static char *output_buffer; // Buffer to store response of http request from event handler
static int output_len;      // Stores number of bytes read
static int current_index = 0;

void xiaozhi_active_init(void)
{
  esp_http_client_config_t config = {
      .host = "api.tenclass.net",
      .path = "/xiaozhi/ota/",
      .transport_type = HTTP_TRANSPORT_OVER_SSL, // 声明使用SSL链接，协议是https
      .event_handler = _http_event_handler,
      // .user_data = 123,
      // .cert_pem = howsmyssl_com_root_cert_pem_start,
      .crt_bundle_attach = esp_crt_bundle_attach, // SSL证书验证
      .method = HTTP_METHOD_POST,                 // 指定请求的方法

  };

  // esp_http_client_set_method(client, HTTP_METHOD_POST);
  // 客户端的初始化
  client = esp_http_client_init(&config);
  // 设置请求头
  xiaozhi_active_set_header();
  // 设置请求体
  xiaozhi_active_set_body();

  // 发起请求， 打开连接，交换数据，关闭连接
  while (esp_http_client_perform(client) != ESP_OK)
  {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    xiaozhi_lvgl_set_title("网络较差...");
    xiaozhi_lvgl_set_emoji("crying");
    xiaozhi_lvgl_set_text("正在积极联网...请稍后");
  }
}

// 设置请求头
void xiaozhi_active_set_header(void)
{
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "User-Agent", "bread-compact-wifi-128x64/1.0.1");
  uint8_t eth_mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, eth_mac);

  sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
  esp_http_client_set_header(client, "Device-Id", mac_str);
  ESP_LOGI(TAG, "Device-Id: %s", mac_str);

  // 7b94d69a-9808-4c59-9c9b-704333b38aff

  gen_uuid(uuid_str);
  ESP_LOGI(TAG, "uuid_str: %s", uuid_str);
  esp_http_client_set_header(client, "Client-Id", uuid_str);
}
// 设置请求体
void xiaozhi_active_set_body(void)
{
  /*
    {
      "application": {
        "version": "1.0.1",
        "elf_sha256": "c8a8ecb6d6fbcda682494d9675cd1ead240ecf38bdde75282a42365a0e396033"
      },
      "board": {
        "type": "bread-compact-wifi",
        "name": "bread-compact-wifi-128x64",
        "ssid": "卧室",
        "rssi": -55,
        "channel": 1,
        "ip": "192.168.1.11",
        "mac": "11:22:33:44:55:66"
      }
    }
  */
  cJSON *root = cJSON_CreateObject();
  cJSON *application = cJSON_CreateObject();
  cJSON *board = cJSON_CreateObject();
  // 给root添加两个成员
  cJSON_AddItemToObject(root, "application", application);
  cJSON_AddItemToObject(root, "board", board);
  // 给application添加成员属性
  cJSON_AddStringToObject(application, "version", "1.0.1");
  cJSON_AddStringToObject(application, "elf_sha256", esp_app_get_elf_sha256_str());
  // 给board添加成员属性
  cJSON_AddStringToObject(board, "type", "bread-compact-wifi");
  cJSON_AddStringToObject(board, "name", "bread-compact-wifi-128x64");
  cJSON_AddStringToObject(board, "ssid", "卧室");
  cJSON_AddNumberToObject(board, "rssi", -55);
  cJSON_AddNumberToObject(board, "channel", 1);
  cJSON_AddStringToObject(board, "ip", "192.168.1.11");
  cJSON_AddStringToObject(board, "mac", mac_str);

  // 将root转换为json字符串
  char *post_data = cJSON_PrintUnformatted(root);

  // 设置请求体
  esp_http_client_set_post_field(client, post_data, strlen(post_data));

  // 删除root
  cJSON_Delete(root);
  // ! 不能在此释放字符中post_data
  // cJSON_free(post_data);
}

void gen_uuid(char *out)
{
  unsigned char b[16];
  for (int i = 0; i < 16; i++)
    b[i] = rand() & 0xFF;
  b[6] = (b[6] & 0x0F) | 0x40; // 版本 4
  b[8] = (b[8] & 0x3F) | 0x80; // 变体
  sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
          b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
          b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{

  switch (evt->event_id)
  {
  case HTTP_EVENT_ERROR:
    ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    if (strcmp(evt->header_key, "Content-Length") == 0)
    {
      output_len = atoi(evt->header_value);
      output_buffer = heap_caps_malloc(output_len + 1, MALLOC_CAP_SPIRAM);
      output_buffer[output_len] = '\0';
    }

    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    // ! 收集服务器端返回的数据
    memcpy(output_buffer + current_index, evt->data, evt->data_len);
    current_index += evt->data_len;
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
    ESP_LOGI(TAG, "Total %d bytes received, output_buffer: %s", output_len, output_buffer);
    // 提取你想要的数据
    xiaozhi_active_parse_json(output_buffer);

    if (output_buffer != NULL)
    {
      // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
      // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
    int mbedtls_err = 0;
    esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
    if (err != 0)
    {
      ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
      ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
    }
    if (output_buffer != NULL)
    {
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    break;
  case HTTP_EVENT_REDIRECT:
    ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
    esp_http_client_set_header(evt->client, "From", "user@example.com");
    esp_http_client_set_header(evt->client, "Accept", "text/html");
    esp_http_client_set_redirection(evt->client);
    break;
  }
  return ESP_OK;
}

void xiaozhi_active_parse_json(char *json_data)
{
  cJSON *root = cJSON_Parse(json_data);
  if (root == NULL)
  {
    ESP_LOGE(TAG, "Error parsing JSON");
    return;
  }

  cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
  if (websocket == NULL)
  {
    ESP_LOGE(TAG, "Error parsing websocket");
    return;
  }

  char *websocket_url = cJSON_GetObjectItem(websocket, "url")->valuestring;
  xiaozhi_handle.websocket_url = heap_caps_calloc(strlen(websocket_url) + 1, sizeof(char), MALLOC_CAP_SPIRAM);

  memcpy(xiaozhi_handle.websocket_url, websocket_url, strlen(websocket_url));

  char *websocket_token = cJSON_GetObjectItem(websocket, "token")->valuestring;
  xiaozhi_handle.websocket_token = heap_caps_calloc(strlen(websocket_token) + 1, sizeof(char), MALLOC_CAP_SPIRAM);
  memcpy(xiaozhi_handle.websocket_token, websocket_token, strlen(websocket_token));

  // 提取激活码
  cJSON *activation = cJSON_GetObjectItem(root, "activation");
  if (activation == NULL)
  {
    ESP_LOGI(TAG, "设备已激活");
    xiaozhi_handle.is_activated = true;
    // 标题由 appmode 统一维护：默认语音模式显示 "AI小智｜语音"，
    // 若激活期间已切到其它模式，这里也不会把标题改错
    xiaozhi_appmode_refresh_ui();
    xiaozhi_lvgl_set_emoji("happy");
    xiaozhi_lvgl_set_text("你好,我是小智");
  }
  else
  {
    ESP_LOGW(TAG, "设备未激活");
    xiaozhi_handle.is_activated = false;
    char *activation_code = cJSON_GetObjectItem(activation, "code")->valuestring;
    xiaozhi_handle.activation_code = heap_caps_calloc(strlen(activation_code) + 1, sizeof(char), MALLOC_CAP_SPIRAM);

    memcpy(xiaozhi_handle.activation_code, activation_code, strlen(activation_code));
    ESP_LOGI(TAG, "activation_code: %s", xiaozhi_handle.activation_code);

    xiaozhi_lvgl_set_title("设备未激活...");
    xiaozhi_lvgl_set_emoji("crying");
    char text_arr[128];
    sprintf(text_arr, "请前往控制台激活 \n 激活码: %s \n 激活后请重启", xiaozhi_handle.activation_code);
    xiaozhi_lvgl_set_text(text_arr);
  }

  ESP_LOGI(TAG, "websocket_url: %s", xiaozhi_handle.websocket_url);
  ESP_LOGI(TAG, "websocket_token: %s", xiaozhi_handle.websocket_token);

  cJSON_Delete(root);
}
