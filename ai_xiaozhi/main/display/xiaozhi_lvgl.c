#include "xiaozhi_lvgl.h"

/* 默认显示界面 */
void xiaozhi_lvgl_show_init(void);

#define TAG "xiaozhi_lvgl"
#define LCD_H_RES (320)
#define LCD_V_RES (240)
#define LCD_DRAW_BUFF_HEIGHT 16

extern esp_lcd_panel_handle_t panel_handle;
extern esp_lcd_panel_io_handle_t io_handle;

extern const lv_font_t font_puhui_14_1;
extern const lv_font_t font_puhui_16_4;
extern const lv_font_t font_puhui_20_4;
/* LVGL 内置字体（sdkconfig 中启用）：montserrat_14 含 LV_SYMBOL_WIFI 图标，
 * montserrat_12 用作时间标签的“小字号”（只有数字/符号，无需中文字形） */
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_12;

static lv_display_t *lvgl_disp = NULL;

lv_obj_t *qrcode = NULL;
lv_obj_t *screen = NULL;

lv_obj_t *title_label = NULL;
lv_obj_t *emoji_label = NULL;
lv_obj_t *text_label = NULL;
/* 标题栏右侧的北京时间（"YYYY-MM-DD HH:MM:SS"），未授时显示 "--" */
lv_obj_t *datetime_label = NULL;
/* 标题栏最右侧的 WiFi 状态图标（不显示文字，靠颜色区分连接中/已连接/失败） */
lv_obj_t *wifi_label = NULL;

/* 标题栏右侧布局：WiFi 图标固定占位宽度（14px 字体下 wifi 图标约 14px），
 * 宽度写死可让左侧时间标签的位置固定，状态切换时不会跳动 */
#define XIAOZHI_LVGL_WIFI_ICON_W 18
#define XIAOZHI_LVGL_TITLE_PAD   4

/* WiFi 状态的缓存：wifi 事件可能早于界面创建（或界面重建），先记住状态 */
static xiaozhi_wifi_state_t s_wifi_state = XIAOZHI_WIFI_STATE_CONNECTING;

typedef struct {
    char *name;
    char *emoji;
} emoji_list_t;

const emoji_list_t emoji_list[21] = {{.name = "neutral", .emoji = "😶"},   {.name = "happy", .emoji = "🙂"},   {.name = "laughing", .emoji = "😆"},
                                     {.name = "funny", .emoji = "😂"},     {.name = "sad", .emoji = "😔"},     {.name = "angry", .emoji = "😠"},
                                     {.name = "crying", .emoji = "😭"},    {.name = "loving", .emoji = "😍"},  {.name = "embarrassed", .emoji = "😳"},
                                     {.name = "surprised", .emoji = "😲"}, {.name = "shocked", .emoji = "😱"}, {.name = "thinking", .emoji = "🤔"},
                                     {.name = "winking", .emoji = "😉"},   {.name = "cool", .emoji = "😎"},    {.name = "relaxed", .emoji = "😌"},
                                     {.name = "delicious", .emoji = "🤤"}, {.name = "kissy", .emoji = "😘"},   {.name = "confident", .emoji = "😏"},
                                     {.name = "sleepy", .emoji = "😴"},    {.name = "silly", .emoji = "😜"},   {.name = "confused", .emoji = "🙄"}};
void xiaozhi_lvgl_init(void)
{
    // LCD初始化
    xiaozhi_lcd_init();
    // LVGL初始化
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,       /* LVGL task priority */
        .task_stack = 8192,       /* LVGL task stack size */
        .task_affinity = -1,      /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500, /* Maximum sleep in LVGL task */
        .timer_period_ms = 5      /* LVGL timer tick period in ms */
    };

    lvgl_port_init(&lvgl_cfg);

    ESP_LOGI(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {.io_handle = io_handle,
                                              .panel_handle = panel_handle,
                                              /* buffer_size 的单位是"像素"，esp_lvgl_port 内部会再乘
                                               * color_bytes(2) 得到字节数。原写法多乘了一次 2，
                                               * 实际每块 = 320*16*2*2 = 20480B，双缓冲白占 40KB
                                               * 内部 DMA RAM；修正后每块 10240B、双缓冲共 20KB，
                                               * 为监控模式的相机（DMA 缓冲/cam_task 栈）腾出 20KB。 */
                                              .buffer_size = LCD_H_RES * LCD_DRAW_BUFF_HEIGHT,
                                              .double_buffer = true,
                                              .hres = LCD_H_RES,
                                              .vres = LCD_V_RES,
                                              .monochrome = false, // 是否使用单通道渲染
                                              .color_format = LV_COLOR_FORMAT_RGB565,
                                              .rotation =
                                                  {
                                                      .swap_xy = true,
                                                      .mirror_x = true,
                                                      .mirror_y = false,
                                                  },
                                              .flags = {
                                                  .buff_dma = true,
                                                  .swap_bytes = true,
                                              }};

    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    // 全局的默认样式
    // 上锁
    lvgl_port_lock(0);
    screen = lv_screen_active();
    // 设置屏幕背景颜色
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xffffff), LV_PART_MAIN);
    // 设置文本默认颜色
    lv_obj_set_style_text_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    // 解锁
    lvgl_port_unlock();

    xiaozhi_lvgl_show_init();
}

/* 显示hello world */
void xiaozhi_lvgl_show_hello_world(void)
{
    // ! lvgl操作屏幕对象是 非原子性
    // 上锁
    lvgl_port_lock(0);
    // 获取屏幕对象
    lv_obj_t *screen = lv_screen_active();

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x003a57), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xffffff), 0);
    // 创建label标签，用于显示文本内容
    lv_obj_t *label = lv_label_create(screen);
    // 设置对齐方式  set_align相对于父元素对齐
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_label_set_text(label, "Hello world");
    // 解锁
    lvgl_port_unlock();
}

/* 显示二维码 */
void xiaozhi_lvgl_show_qrcode(char *qrcode_data)
{
    // 上锁
    lvgl_port_lock(0);
    lv_obj_t *screen = lv_screen_active();
    // 创建一个qrcode对象
    qrcode = lv_qrcode_create(screen);
    // 设置居中
    lv_obj_center(qrcode);
    // 设置大小
    lv_qrcode_set_size(qrcode, 150);
    lv_qrcode_set_dark_color(qrcode, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qrcode, lv_color_hex(0xffffff));
    // 设置二维码的数据
    lv_qrcode_set_data(qrcode, qrcode_data);
    // 给二维码周边设置护城河
    lv_qrcode_set_quiet_zone(qrcode, true);
    // 解锁
    lvgl_port_unlock();
}

/* 删除二维码 */
void xiaozhi_lvgl_delete_qrcode(void)
{
    if (qrcode == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    lv_obj_delete(qrcode);
    // 解锁
    lvgl_port_unlock();
}

/* 按缓存的状态刷新 WiFi 图标的颜色（调用前须已持有 LVGL 锁）
 * 只显示 wifi 小图标、不再显示文字，状态用颜色区分：
 * 灰=连接中/未知，绿=已连接，红=失败/断开 */
static void xiaozhi_lvgl_wifi_apply(void)
{
    if (wifi_label == NULL) {
        return;
    }
    uint32_t color = 0x8A8A8A; /* 灰：连接中/未知 */
    if (s_wifi_state == XIAOZHI_WIFI_STATE_CONNECTED) {
        color = 0x1BA034; /* 绿：连接成功 */
    } else if (s_wifi_state == XIAOZHI_WIFI_STATE_FAILED) {
        color = 0xE53935; /* 红：连接失败/断开 */
    }
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(color), 0);
}

/* 默认显示界面 */
void xiaozhi_lvgl_show_init(void)
{
    // 上锁
    lvgl_port_lock(0);
    // ! 标题区域
    // 创建标题区域容器
    lv_obj_t *title_container = lv_obj_create(screen);
    // 设置圆角
    lv_obj_set_style_radius(title_container, 0, LV_PART_MAIN);
    // 设置滚动条
    lv_obj_set_scrollbar_mode(title_container, LV_SCROLLBAR_MODE_OFF);
    // 设置边框
    lv_obj_set_style_border_width(title_container, 0, LV_PART_MAIN);
    // 设置大小
    lv_obj_set_size(title_container, LCD_H_RES, 30);
    // 设置背景颜色
    lv_obj_set_style_bg_color(title_container, lv_color_hex(0xBFCADB), 0);
    // 创建label标签
    title_label = lv_label_create(title_container);
    // 设置字体
    lv_obj_set_style_text_font(title_label, &font_puhui_14_1, 0);
    // 设置label的文本（默认进入语音模式，标题与 appmode 的语音模式标题一致）
    lv_label_set_text(title_label, "AI小智｜语音");
    // 标题靠左，时间/WiFi 标识靠右，共用 320x30 的标题条，互不遮挡
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 6, 0);

    // ! 标题栏最右侧的 WiFi 状态图标（时间标签排在它左边）
    wifi_label = lv_label_create(title_container);
    // montserrat_14 内含 LV_SYMBOL_WIFI 图标字形，只显示图标不显示文字
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_14, 0);
    // 固定宽度 + 右对齐，状态切换时图标位置不跳动，也不换行溢出标题条
    lv_obj_set_width(wifi_label, XIAOZHI_LVGL_WIFI_ICON_W);
    lv_obj_set_style_text_align(wifi_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(wifi_label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_align(wifi_label, LV_ALIGN_RIGHT_MID, -XIAOZHI_LVGL_TITLE_PAD, 0);

    // ! 标题栏右侧的北京时间（由 xiaozhi_time 每秒刷新，未授时显示 "--"）
    datetime_label = lv_label_create(title_container);
    // 时间用更小的 12px 字体（纯数字/符号，不需要中文字形），给标题留出空间
    lv_obj_set_style_text_font(datetime_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(datetime_label, "--");
    // 排在 WiFi 图标左侧，两者之间留 XIAOZHI_LVGL_TITLE_PAD 的间隔
    lv_obj_align(datetime_label, LV_ALIGN_RIGHT_MID,
                 -(XIAOZHI_LVGL_TITLE_PAD * 2 + XIAOZHI_LVGL_WIFI_ICON_W), 0);

    // 按缓存状态先渲染一次（配网/WiFi 事件可能早于界面创建完成）
    xiaozhi_lvgl_wifi_apply();

    // ! emoji区域
    // 创建label标签
    emoji_label = lv_label_create(screen);
    // 设置宽度
    lv_obj_set_width(emoji_label, 320);
    // 设置文本居中
    lv_obj_set_style_text_align(emoji_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(emoji_label, font_emoji_64_init(), 0);
    lv_label_set_text(emoji_label, "🙂");
    // 设置相对对齐方式
    lv_obj_align_to(emoji_label, title_container, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);
    // ! 文本区域
    // 创建label标签
    text_label = lv_label_create(screen);
    // 设置宽度
    lv_obj_set_width(text_label, 280);
    // 设置文本居中
    lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
    // 设置字体
    lv_obj_set_style_text_font(text_label, &font_puhui_14_1, 0);
    lv_label_set_text(text_label, "你好，我是小智");
    // 设置相对对齐方式
    lv_obj_align_to(text_label, emoji_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 30);
    // 解锁
    lvgl_port_unlock();
}

/* 修改标题的方法 */
void xiaozhi_lvgl_set_title(char *title)
{
    // 界面未初始化（title_label 还未创建）时直接返回，避免早期调用崩溃
    if (title_label == NULL || title == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    lv_label_set_text(title_label, title);
    // 解锁
    lvgl_port_unlock();
}
/* 修改文本的方法 */
void xiaozhi_lvgl_set_text(char *text)
{
    // 上锁
    lvgl_port_lock(0);
    lv_label_set_text(text_label, text);
    // 解锁
    lvgl_port_unlock();
}
/* 修改emoji的方法 */
void xiaozhi_lvgl_set_emoji(char *emoji_name)
{
    bool is_find = false;
    // 上锁
    lvgl_port_lock(0);
    for (int i = 0; i < 21; i++) {
        if (strcmp(emoji_name, emoji_list[i].name) == 0) {
            lv_label_set_text(emoji_label, emoji_list[i].emoji);
            is_find = true;
            break;
        }
    }

    if (!is_find) {
        lv_label_set_text(emoji_label, "🙂");
    }
    // 解锁
    lvgl_port_unlock();
}

/* 修改标题栏右侧的北京时间（"YYYY-MM-DD HH:MM:SS"） */
void xiaozhi_lvgl_set_datetime(const char *datetime)
{
    // 界面未初始化 / 传入空指针时直接返回，避免早期调用崩溃
    if (datetime_label == NULL || datetime == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    lv_label_set_text(datetime_label, datetime);
    // 解锁
    lvgl_port_unlock();
}

/* 更新标题栏最右侧的 WiFi 状态标识（由 xiaozhi_wifi 的 wifi/ip 事件驱动） */
void xiaozhi_lvgl_set_wifi_state(xiaozhi_wifi_state_t state)
{
    if (state != XIAOZHI_WIFI_STATE_CONNECTING && state != XIAOZHI_WIFI_STATE_CONNECTED &&
        state != XIAOZHI_WIFI_STATE_FAILED) {
        return;
    }
    // 先缓存状态：界面未初始化（wifi_label 还没创建）时也要记下来，show_init 会补显示
    s_wifi_state = state;
    if (wifi_label == NULL) {
        return;
    }
    // 上锁
    lvgl_port_lock(0);
    xiaozhi_lvgl_wifi_apply();
    // 解锁
    lvgl_port_unlock();
}