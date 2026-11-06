#include "xiaozhi_sr.h"

#include "xiaozhi_common.h"

/* feed 数据的任务 */
void xiaozhi_sr_feed_task(void *arg);
/* fetch 数据的任务 */
void xiaozhi_sr_fetch_task(void *arg);

static const esp_afe_sr_iface_t *afe_handle = NULL;
esp_afe_sr_data_t *afe_data = NULL;

#define TAG "xiaozhi_sr"



void xiaozhi_sr_init(void)
{

  srmodel_list_t *models = esp_srmodel_init("model");
  afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
  afe_config->vad_min_noise_ms = 1000; // 判断静默的最小延时时间
  afe_config->vad_min_speech_ms = 128; // 判断是否是人声的最短延时时间
  afe_config->vad_mode = VAD_MODE_1;   // 级别越高，对语音检测的音频数据要求越高，
  // ! 解决ringbuff is full的问题
  afe_config->afe_ringbuf_size = 6 * 1024;

  // 关闭一些硬件上本来就不支持的功能 免得出现反效果
  afe_config->aec_init = false; // 禁用回声消除
  afe_config->se_init = false;  // 禁用人声增强
  afe_config->ns_init = false;  // 禁用噪声消除

  // 提升了唤醒的灵敏度
  afe_config->wakenet_mode = DET_MODE_90;
  // 更多的内存分配到外部的sram里去
  afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

  afe_handle = esp_afe_handle_from_config(afe_config);
  afe_data = afe_handle->create_from_config(afe_config);

  // 注册任务
  xiaozhi_task_create(xiaozhi_sr_feed_task, "xiaozhi_sr_feed_task", XIAOZHI_STACK_32K, NULL, XIAOZHI_PRIO_AUDIO, NULL, 1);
  xiaozhi_task_create(xiaozhi_sr_fetch_task, "xiaozhi_sr_fetch_task", XIAOZHI_STACK_32K, NULL, XIAOZHI_PRIO_AUDIO, NULL, 1);
}

/* feed 数据的任务 */
void xiaozhi_sr_feed_task(void *arg)
{
  // 获取喂数据的chunk块大小
  int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
  // 获取喂数据的通道数量
  int feed_channel = afe_handle->get_feed_channel_num(afe_data);
  // 动态分配喂数据的缓冲区大小
  int16_t *i2s_buff = heap_caps_malloc(audio_chunksize * sizeof(int16_t) * feed_channel, MALLOC_CAP_SPIRAM);
  while (1)
  {
    // 示例项目中使用的假数据
    // esp_get_feed_data(true, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
    // 问ES8311要数据
    xiaozhi_audio_record((uint8_t *)i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
    afe_handle->feed(afe_data, i2s_buff);
  }
}

/* fetch 数据的任务 */
void xiaozhi_sr_fetch_task(void *arg)
{
  while (1)
  {
    // 通过fetch提取处理音频和语音活动检测的结果
    afe_fetch_result_t *result = afe_handle->fetch(afe_data);
    // 从SR的模型中提取的音频数据
    int16_t *processed_audio = result->data;
    //  打印提取数据的大小
    // ESP_LOGI(TAG, "fetch_data_size: %d", result->data_size);
    // vad的语音状态， 1. SLIENCE  2. SPEECH
    vad_state_t vad_state = result->vad_state;
    // 唤醒的状态， 关心：WAKENET_DETECTED
    wakenet_state_t wakeup_state = result->wakeup_state;

    if (wakeup_state == WAKENET_DETECTED)
    {
      ESP_LOGI(TAG, "检测到了唤醒词...");
      xiaozhi_handle.is_wakeup = true;
      // 初始化两个vad的状态
      xiaozhi_handle.vad_previous_state = VAD_SILENCE;
      xiaozhi_handle.vad_current_state = VAD_SILENCE;

      if (xiaozhi_handle.wakeup_callback != NULL)
      {
        xiaozhi_handle.wakeup_callback();
      }
    }

    if (xiaozhi_handle.is_wakeup)
    {
      xiaozhi_handle.vad_current_state = vad_state;
      // 判断vad的状态发生改变
      if (xiaozhi_handle.vad_current_state != xiaozhi_handle.vad_previous_state)
      {
        if (xiaozhi_handle.vad_change_callback != NULL)
        {
          xiaozhi_handle.vad_change_callback();
        }
      }
      // 更新上一次的状态
      xiaozhi_handle.vad_previous_state = xiaozhi_handle.vad_current_state;
    }

    /* 思考： 什么时候将数据放入至环形缓冲区
      1. 检测到被唤醒了
      2. 检测到人声
    */
    if (xiaozhi_handle.is_wakeup && xiaozhi_handle.vad_current_state == VAD_SPEECH)
    {

      // if vad cache is exists, please attach the cache to the front of processed_audio to avoid data loss
      //需要判断环形缓冲区是否满了
      if (result->vad_cache_size > 0)
      {
        // 如果有缓冲数据，就将缓冲数据也放入至环形缓冲区，注意：一定要在正常的处理音频之前放入
        xRingbufferSend(xiaozhi_handle.ring_buffer_sr2encoder_handle, result->vad_cache, result->vad_cache_size, portMAX_DELAY);
      }
      // 将PCM音频数据放入至sr2encoder的环形缓冲区
      xRingbufferSend(xiaozhi_handle.ring_buffer_sr2encoder_handle, processed_audio, result->data_size, portMAX_DELAY);
    }
  }
}
