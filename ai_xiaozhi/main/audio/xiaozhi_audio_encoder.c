#include "xiaozhi_audio_encoder.h"

#include "xiaozhi_common.h"

/* 编码器的任务 */
void xiaozhi_audio_encoder_task(void *pvParameters);

#define TAG "xiaozhi_audio_encoder"

esp_audio_enc_handle_t encoder_handle = NULL;
int input_pcm_size = 0;
int output_opus_size = 0;
uint8_t *input_pcm_data = NULL;
uint8_t *output_opus_data = NULL;
void xiaozhi_audio_encoder_init(void)
{
  esp_opus_enc_config_t opus_cfg = {
      .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
      .channel = ESP_AUDIO_MONO,
      .bits_per_sample = ESP_AUDIO_BIT16,
      .bitrate = 32000, // 比特率的快慢决定了音频质量，比特率越高占用CPU的资源越多
      .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
      .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
      .complexity = 5,     // 编码器的复杂程度， 复杂程度越高，转换的opus音频质量越高  0 ~ 10
      .enable_fec = false, // 前向纠错
      .enable_dtx = false,
      .enable_vbr = false, // 可变比特率  提高传递的效率， 但是严重占用CPU的资源
  };
  esp_opus_enc_open(&opus_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);

  esp_opus_enc_get_frame_size(encoder_handle, &input_pcm_size, &output_opus_size);

  ESP_LOGI(TAG, "input_pcm_size:%d, output_opus_size:%d", input_pcm_size, output_opus_size);

  // 为encoder输入和输出分配内存
  input_pcm_data = heap_caps_malloc(input_pcm_size, MALLOC_CAP_SPIRAM);
  output_opus_data = heap_caps_malloc(output_opus_size, MALLOC_CAP_SPIRAM);

  // 注册编码器的任务
  xiaozhi_task_create(xiaozhi_audio_encoder_task, "xiaozhi_audio_encoder_task", XIAOZHI_STACK_32K, NULL, XIAOZHI_PRIO_AUDIO, NULL, 1);
}

/* 编码器的任务 */
void xiaozhi_audio_encoder_task(void *pvParameters)
{
  esp_audio_enc_in_frame_t in_frame = {
      .buffer = input_pcm_data,
      .len = input_pcm_size,
  };
  esp_audio_enc_out_frame_t out_frame = {
      .buffer = output_opus_data,
      .len = output_opus_size,
  };
  while (1)
  {
    // 从环形缓冲区中获取数据
    /* 环形缓冲区的情况
      1. 0
      2. >= maxSize
      3. >0 && < maxSize
    */
    size_t remain_size = in_frame.len;
    uint8_t *temp_pointer = in_frame.buffer;
    while (remain_size > 0)
    {
      size_t rev_size = 0;
      // xRingbufferReceiveUpTo放行情况： 1. 超时  2. 取到了maxSize的数据  3. 取到了数据，但是<maxSize的数据
      void *rev_pointer = xRingbufferReceiveUpTo(xiaozhi_handle.ring_buffer_sr2encoder_handle, &rev_size, portMAX_DELAY, remain_size);
      memcpy(temp_pointer, rev_pointer, rev_size);
      temp_pointer += rev_size;
      remain_size -= rev_size;

      // ! 释放环形缓冲区的数据
      vRingbufferReturnItem(xiaozhi_handle.ring_buffer_sr2encoder_handle, rev_pointer);
    }
    // 开始编码
    esp_opus_enc_process(encoder_handle, &in_frame, &out_frame);
    // 将编码之后的opus数据放入至encoder2ws环形缓冲区    out_frame.encoded_bytes是实际编码的字节数
    xRingbufferSend(xiaozhi_handle.ring_buffer_encoder2ws_handle, out_frame.buffer, out_frame.encoded_bytes, portMAX_DELAY);
  }
}