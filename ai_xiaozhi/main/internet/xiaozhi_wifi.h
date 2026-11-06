#pragma once

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include <wifi_provisioning/scheme_ble.h>
#include <wifi_provisioning/manager.h>

#include "qrcode.h"
#include "xiaozhi_lvgl.h"

void xiaozhi_wifi_init(void);



/* 清除配网信息 */
void xiaozhi_wifi_reset(void);