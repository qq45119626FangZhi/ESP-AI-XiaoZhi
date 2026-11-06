#pragma once
#include "stdlib.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* 定义小智的状态 */
typedef enum
{
  XIAOZHI_STATE_IDLE,
  XIAOZHI_STATE_LISTENING,
  XIAOZHI_STATE_SPEAKING,
} xiaozhi_state_t;

typedef struct
{
  // websocket url
  char *websocket_url;
  // websocket token
  char *websocket_token;
  // 激活码
  char *activation_code;
  // 是否激活的标志
  bool is_activated;
  // 被唤醒的回调
  void (*wakeup_callback)(void);
  // 被唤醒的标志
  bool is_wakeup;
  // vad状态发生改变的回调
  void (*vad_change_callback)(void);
  // vad的当前状态
  vad_state_t vad_current_state;
  // vad的上一个状态
  vad_state_t vad_previous_state;

  // sr2encoder的环形缓冲区
  RingbufHandle_t ring_buffer_sr2encoder_handle;
  // encoder2ws的环形缓冲区
  RingbufHandle_t ring_buffer_encoder2ws_handle;
  // ws2decoder的环形缓冲区
  RingbufHandle_t ring_buffer_ws2decoder_handle;

  /* 定义和ws相关的事件标志组 */
  EventGroupHandle_t ws_event_group;

  /* 处理文本帧数据的方法 */
  void (*process_text_frame)(char *text_frame);
  /* 处理二进制音频数据帧的方法 */
  void (*process_binary_audio_frame)(const char *bin_data, int data_len);

  /* 小智的状态 */
  xiaozhi_state_t xiaozhi_state;

} xiaozhi_handle_t;

// ws客户端和服务器连接成功的掩码
#define WS_CONNECTED_BIT BIT0
// 发送hello成功的标志位掩码
#define WS_HELLO_SENT_BIT BIT1

extern xiaozhi_handle_t xiaozhi_handle;

/* ==========================================================================
 * 统一的资源规格枚举（栈 / 优先级 / 环形缓冲）
 *
 * 为什么要集中在这里：
 *   本板内部 SRAM 要优先留给 DMA 类分配（mbedtls 硬件 AES-DMA、摄像头 DMA、
 *   LVGL 刷屏缓冲、I2S 音频 DMA）。一旦监控模式 + 语音对话同时运行，内部 RAM
 *   很容易被任务栈吃掉，实测会出现
 *     E esp-aes: Failed to allocate memory
 *     E esp-tls-mbedtls: write error
 *   随后 WebSocket 断开、小智无法唤醒。
 *   CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y 已打开，因此所有业务任务栈
 *   一律放到 PSRAM（外扩 SPI RAM），内部 RAM 只留 DMA 与中断上下文所需部分。
 *
 * 使用约定：
 *   1. 新建任务一律用 xiaozhi_task_create()，并用下面枚举传栈大小/优先级，
 *      不要再写魔法数字，也不要用 xTaskCreatePinnedToCore() 直接创建
 *      （那样栈会落在内部 RAM）。
 *   2. 任务体内不得调用会禁用 cache 的操作（spi_flash 写、nvs_commit 等），
 *      否则 PSRAM 栈会失效；SD 卡 FATFS、SNTP、RMT、I2S 都是安全的。
 *   3. 环形缓冲区一律用 xiaozhi_ringbuf_create()，缓冲本体分配在 PSRAM。
 * ========================================================================== */

/* 任务栈大小（单位：字节，与 FreeRTOS 的 usStackDepth 一致） */
typedef enum
{
    XIAOZHI_STACK_2K  = 2048,
    XIAOZHI_STACK_2K5 = 2560,
    XIAOZHI_STACK_4K  = 4096,
    XIAOZHI_STACK_5K  = 5120,
    XIAOZHI_STACK_6K  = 6144,
    XIAOZHI_STACK_8K  = 8192,
    XIAOZHI_STACK_16K = 16384,
    XIAOZHI_STACK_32K = 32768,
} xiaozhi_task_stack_t;

/* 任务优先级（configMAX_PRIORITIES = 25，数值越大优先级越高） */
typedef enum
{
    XIAOZHI_PRIO_LOWEST = 1, /* SD 卡守护（只做低频探测） */
    XIAOZHI_PRIO_LOW    = 2, /* 录像写盘、状态灯跟随 */
    XIAOZHI_PRIO_NORMAL = 3, /* 摄像头预览 */
    XIAOZHI_PRIO_HIGH   = 4, /* 模式仲裁、LVGL */
    XIAOZHI_PRIO_AUDIO  = 5, /* 语音链路（SR/编码/解码/WS 发送） */
} xiaozhi_task_prio_t;

/* 环形缓冲区容量（单位：字节） */
typedef enum
{
    XIAOZHI_RINGBUF_8K  = 8 * 1024,
    XIAOZHI_RINGBUF_16K = 16 * 1024,
    XIAOZHI_RINGBUF_32K = 32 * 1024,
} xiaozhi_ringbuf_size_t;

/* 统一的任务创建：栈一律优先分配在 PSRAM，失败自动回落内部 RAM。
 * 返回值与 xTaskCreate 一致（pdPASS / errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY）。 */
BaseType_t xiaozhi_task_create(TaskFunction_t task_func, const char *name, xiaozhi_task_stack_t stack,
                               void *arg, xiaozhi_task_prio_t prio, TaskHandle_t *handle, int core_id);

/* 统一的环形缓冲区创建：缓冲本体分配在 PSRAM（失败回落内部 RAM）。 */
RingbufHandle_t xiaozhi_ringbuf_create(xiaozhi_ringbuf_size_t size, RingbufferType_t type);
