#pragma once
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

void xiaozhi_audio_init(void);

/* 播放音频的功能函数 */
void xiaozhi_audio_play(uint8_t *data, size_t size);

/* 录音的功能函数 */
void xiaozhi_audio_record(uint8_t *data, size_t size);

void xiaozhi_audio_volume_set(int volume);

/* 设置是否静音 */
void xiaozhi_audio_mute_set(bool mute);
