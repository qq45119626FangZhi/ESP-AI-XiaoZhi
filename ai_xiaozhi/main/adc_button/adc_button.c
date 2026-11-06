#include "adc_button.h"

#define TAG "adc_button"

// 句柄对象
button_handle_t sw2_adc_btn = NULL;
// 句柄对象
button_handle_t sw3_adc_btn = NULL;
void adc_button_init(void)
{
  // ! SW2配置
  // btn 配置项
  const button_config_t sw2_btn_cfg = {0};
  // adc配置项
  button_adc_config_t sw2_btn_adc_cfg = {
      .unit_id = ADC_UNIT_1, // 具体使用哪个ADC需要根据硬件原理图的GPIO引脚确认
      .adc_channel = 7,
      .button_index = 0, // 每个按键的index应该是唯一的
      .min = 0,
      .max = 500, // 单位是mv
  };

  iot_button_new_adc_device(&sw2_btn_cfg, &sw2_btn_adc_cfg, &sw2_adc_btn);
  if (NULL == sw2_adc_btn)
  {
    ESP_LOGE(TAG, "Button create failed");
  }
  // ! sw3配置
  // btn 配置项
  const button_config_t sw3_btn_cfg = {0};
  // adc配置项
  button_adc_config_t sw3_btn_adc_cfg = {
      .unit_id = ADC_UNIT_1, // 具体使用哪个ADC需要根据硬件原理图的GPIO引脚确认
      .adc_channel = 7,
      .button_index = 1, // 每个按键的index应该是唯一的
      .min = 1300,
      .max = 1800, // 单位是mv
  };

  iot_button_new_adc_device(&sw3_btn_cfg, &sw3_btn_adc_cfg, &sw3_adc_btn);
  if (NULL == sw3_adc_btn)
  {
    ESP_LOGE(TAG, "Button create failed");
  }
}

/* 给sw2注册回调 */
void adc_button_register_sw2_callback(button_event_t event_type, button_cb_t button_cb, void *user_data)
{
  iot_button_register_cb(sw2_adc_btn, event_type, NULL, button_cb, user_data);
}
/* 给sw3注册回调 */
void adc_button_register_sw3_callback(button_event_t event_type, button_cb_t button_cb, void *user_data)
{
  iot_button_register_cb(sw3_adc_btn, event_type, NULL, button_cb, user_data);
}
