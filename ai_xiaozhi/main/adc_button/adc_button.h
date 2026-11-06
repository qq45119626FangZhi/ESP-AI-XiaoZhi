#pragma once
#include "button_adc.h"
#include "iot_button.h"
#include "esp_log.h"

void adc_button_init(void);

/* 给sw2注册回调 */
void adc_button_register_sw2_callback(button_event_t event_type, button_cb_t button_cb, void *user_data);
/* 给sw3注册回调 */
void adc_button_register_sw3_callback(button_event_t event_type, button_cb_t button_cb, void *user_data);