#include "xiaozhi_audio_decoder.h"

#include "xiaozhi_common.h"

/* 解码器的任务 */
void xiaozhi_audio_decoder_task(void *pvParameters);

esp_audio_dec_handle_t decoder_handle;
void xiaozhi_audio_decoder_init(void)
{
  esp_opus_dec_cfg_t opus_cfg = {
      .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
      .channel = ESP_AUDIO_MONO,
      .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
      .self_delimited = false, // 用于处理多种音频数据的
  };
  esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &decoder_handle);

  // 注册解码器的任务
  xiaozhi_task_create(xiaozhi_audio_decoder_task, "xiaozhi_audio_decoder_task", XIAOZHI_STACK_32K, NULL, XIAOZHI_PRIO_AUDIO, NULL, 1);
}

/* 解码器的任务 */
void xiaozhi_audio_decoder_task(void *pvParameters)
{
  esp_audio_dec_in_raw_t in_frame = {0};
  esp_audio_dec_out_frame_t out_frame = {
      .buffer = heap_caps_malloc(8 * 1024, MALLOC_CAP_SPIRAM),
      .len = 8 * 1024,
  };
  esp_audio_dec_info_t aud_info;
  while (1)
  {
    // 从ws2decoder的环形缓冲区中提取数据（xRingbufferReceive 需要 size_t*，故用临时变量中转）
    size_t item_size = 0;
    void *rev_pointer = xRingbufferReceive(xiaozhi_handle.ring_buffer_ws2decoder_handle, &item_size, portMAX_DELAY);
    in_frame.buffer = rev_pointer;
    in_frame.len = (uint32_t)item_size;
    while (in_frame.len > 0)
    {
      // 开始解码
      esp_opus_dec_decode(decoder_handle, &in_frame, &out_frame, &aud_info);
      in_frame.len -= in_frame.consumed;
      in_frame.buffer += in_frame.consumed;

      // 将解码以后的数据交给ES8311播放
      xiaozhi_audio_play(out_frame.buffer, out_frame.decoded_size);
    }

    // ! 释放环形缓冲区
    vRingbufferReturnItem(xiaozhi_handle.ring_buffer_ws2decoder_handle, rev_pointer);
  }
}