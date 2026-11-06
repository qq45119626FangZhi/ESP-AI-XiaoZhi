#include "xiaozhi_wifi.h"

void xiaozhi_wifi_nvs_init(void);
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void get_device_service_name(char *service_name, size_t max);
static void wifi_prov_print_qr(const char *name, const char *username, const char *pop, const char *transport);

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static int retries = 0;

#define TAG "xiaozhi_wifi"
#define WIFI_CONNECTED_BIT BIT0
#define PROV_QR_VERSION "v1"
#define PROV_TRANSPORT_BLE "ble"
void xiaozhi_wifi_init(void)
{
  // NVS初始化
  xiaozhi_wifi_nvs_init();

  // WIFI初始化
  // 创建事件标志组
  s_wifi_event_group = xEventGroupCreate();
  // net interface
  ESP_ERROR_CHECK(esp_netif_init());
  // 创建event loop的任务
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  // 创建默认的sta接口
  esp_netif_create_default_wifi_sta();
  // wifi初始化配置生效
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_got_ip));
  // ! ------------------------------------- 一下是新增的代码 -------------------------------------
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

  wifi_prov_mgr_config_t config = {
      .scheme = wifi_prov_scheme_ble,                                      // 声明使用BLE
      .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM // 声明配网结束以后，清除和BLE相关的数据
  };
  // 配网的初始化配置生效
  wifi_prov_mgr_init(config);

  bool provisioned = false;
  // 检查是否已经配网
  wifi_prov_mgr_is_provisioned(&provisioned);

  // 没有配网
  if (!provisioned)
  {
    ESP_LOGI(TAG, "Starting provisioning");

    char service_name[12]; // PROV_010203
    // 获取设备名称： MAC地址 PROV_010203
    get_device_service_name(service_name, sizeof(service_name));
    // 设置加密的版本是1， 对称加密
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

    // 加密的密钥
    const char *pop = "abcd1234";
    wifi_prov_security1_params_t *sec_params = pop;

    const char *username = NULL;
    const char *service_key = NULL;

    uint8_t custom_service_uuid[] = {
        /* LSB <---------------------------------------
         * ---------------------------------------> MSB */
        0xb4,
        0xdf,
        0x5a,
        0x1c,
        0x3f,
        0x6b,
        0xf4,
        0xbf,
        0xea,
        0x4a,
        0x82,
        0x03,
        0x04,
        0x90,
        0x1a,
        0x02,
    };

    // 设置设备的UUID， UUID可以作为当前设备的唯一标识
    wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
    // 开始配网
    wifi_prov_mgr_start_provisioning(security, (const void *)sec_params, service_name, service_key);
    // 将二维码输出在控制台
    wifi_prov_print_qr(service_name, username, pop, PROV_TRANSPORT_BLE);
  }
  // 已经配网
  else
  {
    ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");
    // 清除之前的配网初始化相关数据
    wifi_prov_mgr_deinit();
  }

  // wifi_config_t wifi_config = {
  //     .sta = {
  //         .ssid = "闫志勇的iPhone",
  //         .password = "123yzy123",
  //         /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
  //          * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
  //          * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
  //          * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
  //          */
  //         .threshold.authmode = WIFI_AUTH_WPA2_PSK, // 要求密钥长度有限制 8 ~ 64位
  //         .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
  //         .sae_h2e_identifier = "",
  //     },
  // };
  // 设置模式为station
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  // 让配置生效
  // ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  // 启动wifi
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished");

  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT,
                                         pdTRUE,
                                         pdFALSE,
                                         portMAX_DELAY);
  if (bits & WIFI_CONNECTED_BIT)
  {
    ESP_LOGI(TAG, "wifi connected...");
  }
}

void xiaozhi_wifi_nvs_init(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
  // ! ---------------------- WIFI_EVENT ----------------------
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    // 发起链接
    esp_wifi_connect();
    // 标题栏 WiFi 标识：连接中
    xiaozhi_lvgl_set_wifi_state(XIAOZHI_WIFI_STATE_CONNECTING);
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    // 标题栏 WiFi 标识：失败/断开（重连成功后会由 GOT_IP 改回“已连接”）
    xiaozhi_lvgl_set_wifi_state(XIAOZHI_WIFI_STATE_FAILED);
    // 自动重连
    if (s_retry_num < 5)
    {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    }
  }
  // ! ---------------------- IP_EVENT ----------------------
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    // 标题栏 WiFi 标识：连接成功
    xiaozhi_lvgl_set_wifi_state(XIAOZHI_WIFI_STATE_CONNECTED);
    // ! 删除配网的二维码
    xiaozhi_lvgl_delete_qrcode();
    s_retry_num = 0;
    // 一旦获取到IP说明联网成功，主动修改事件标志组的对应的标志位
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
  // ! ---------------------- WIFI_PROV_EVENT ----------------------
  if (event_base == WIFI_PROV_EVENT)
  {
    switch (event_id)
    {
    case WIFI_PROV_START:
      ESP_LOGI(TAG, "Provisioning started");
      break;
    case WIFI_PROV_CRED_RECV:
    {
      wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
      ESP_LOGI(TAG, "Received Wi-Fi credentials"
                    "\n\tSSID     : %s\n\tPassword : %s",
               (const char *)wifi_sta_cfg->ssid,
               (const char *)wifi_sta_cfg->password);
      break;
    }
    case WIFI_PROV_CRED_FAIL:
    {
      wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
      ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                    "\n\tPlease reset to factory and retry provisioning",
               (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
      retries++;
      if (retries >= 5)
      {
        ESP_LOGI(TAG, "Failed to connect with provisioned AP, resetting provisioned credentials");
        wifi_prov_mgr_reset_sm_state_on_failure();
        retries = 0;
      }
      break;
    }
    case WIFI_PROV_CRED_SUCCESS:
      ESP_LOGI(TAG, "Provisioning successful");
      retries = 0;
      break;
    case WIFI_PROV_END:
      /* De-initialize manager once provisioning is finished */
      wifi_prov_mgr_deinit();
      break;
    default:
      break;
    }
  }
  // ! ---------------------- PROTOCOMM_TRANSPORT_BLE_EVENT ----------------------
  if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT)
  {
    switch (event_id)
    {
    case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
      ESP_LOGI(TAG, "BLE transport: Connected!");
      break;
    case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
      ESP_LOGI(TAG, "BLE transport: Disconnected!");
      break;
    default:
      break;
    }
  }
  // ! ---------------------- PROTOCOMM_SECURITY_SESSION_EVENT ----------------------

  if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT)
  {
    switch (event_id)
    {
    case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
      ESP_LOGI(TAG, "Secured session established!");
      break;
    case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
      ESP_LOGE(TAG, "Received invalid security parameters for establishing secure session!");
      break;
    case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
      ESP_LOGE(TAG, "Received incorrect username and/or PoP for establishing secure session!");
      break;
    default:
      break;
    }
  }
}

static void get_device_service_name(char *service_name, size_t max)
{
  uint8_t eth_mac[6];
  const char *ssid_prefix = "PROV_";
  esp_wifi_get_mac(WIFI_IF_STA, eth_mac); // PROV_010203
  snprintf(service_name, max, "%s%02X%02X%02X",
           ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

static void wifi_prov_print_qr(const char *name, const char *username, const char *pop, const char *transport)
{
  if (!name || !transport)
  {
    ESP_LOGW(TAG, "Cannot generate QR code payload. Data missing.");
    return;
  }
  char payload[150] = {0};
  if (pop)
  {
    snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\""
                                       ",\"pop\":\"%s\",\"transport\":\"%s\"}",
             PROV_QR_VERSION, name, pop, transport);
  }
  else
  {
    snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\""
                                       ",\"transport\":\"%s\"}",
             PROV_QR_VERSION, name, transport);
  }

  ESP_LOGI(TAG, "Scan this QR code from the provisioning application for Provisioning.");
  // ! 显示二维码至控制台
  // esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  // esp_qrcode_generate(&cfg, payload);
  // ! 提示用户扫码配网
  xiaozhi_lvgl_set_title("请扫码配网");
  // ! 显示二维码至LCD屏幕
  xiaozhi_lvgl_show_qrcode(payload);
}

/* 清除配网信息 */
void xiaozhi_wifi_reset(void)
{
  // 清除配网信息
  esp_wifi_restore();
  // 重启整个程序
  esp_restart();
  ESP_LOGI(TAG, "清除配网信息...");
}