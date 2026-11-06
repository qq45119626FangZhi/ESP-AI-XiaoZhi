#pragma once
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "string.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_tls.h"
#include "xiaozhi_common.h"
#include "xiaozhi_lvgl.h"

void xiaozhi_active_init(void);