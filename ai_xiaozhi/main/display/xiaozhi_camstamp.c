#include "xiaozhi_camstamp.h"

#if XIAOZHI_CAMSTAMP_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "xiaozhi_time.h"

/* 点阵尺寸 */
#define CAMSTAMP_GLYPH_W   5
#define CAMSTAMP_GLYPH_H   7

/* 渲染用字符串缓冲："YYYY-MM-DD HH:MM:SS" = 19 字符 + '\0' */
#define CAMSTAMP_TEXT_MAX  24

/* 5x7 点阵：每个字节是一列，bit0 = 最上一行（只放时间用得到的字符） */
typedef struct {
    char ch;
    uint8_t col[CAMSTAMP_GLYPH_W];
} camstamp_glyph_t;

static const camstamp_glyph_t s_glyphs[] = {
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
};

static const uint8_t *camstamp_glyph(char ch)
{
    for (size_t i = 0; i < sizeof(s_glyphs) / sizeof(s_glyphs[0]); i++) {
        if (s_glyphs[i].ch == ch) {
            return s_glyphs[i].col;
        }
    }
    return NULL;
}

/* 整串宽度（像素）：len 个字符 + (len-1) 个间距 */
static int camstamp_text_width(int len, int scale)
{
    if (len <= 0) {
        return 0;
    }
    return len * CAMSTAMP_GLYPH_W * scale + (len - 1) * XIAOZHI_CAMSTAMP_GAP * scale;
}

/* 50% 黑底：每个通道右移一位（掩码防止通道之间串位） */
static void camstamp_darken_box(uint16_t *buf, int w, int h, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > w) {
        x1 = w;
    }
    if (y1 > h) {
        y1 = h;
    }
    for (int y = y0; y < y1; y++) {
        uint16_t *row = buf + (size_t)y * (size_t)w;
        for (int x = x0; x < x1; x++) {
            row[x] = (uint16_t)((row[x] & 0xF7DE) >> 1);
        }
    }
}

/* 画一个字符（scale 倍放大，越界自动裁剪） */
static void camstamp_draw_glyph(uint16_t *buf, int w, int h, int x, int y, char ch, int scale)
{
    const uint8_t *cols = camstamp_glyph(ch);
    if (cols == NULL) {
        return;
    }
    for (int cx = 0; cx < CAMSTAMP_GLYPH_W; cx++) {
        uint8_t bits = cols[cx];
        for (int cy = 0; cy < CAMSTAMP_GLYPH_H; cy++) {
            if ((bits & (1u << cy)) == 0) {
                continue;
            }
            for (int sy = 0; sy < scale; sy++) {
                int py = y + cy * scale + sy;
                if (py < 0 || py >= h) {
                    continue;
                }
                uint16_t *row = buf + (size_t)py * (size_t)w;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + cx * scale + sx;
                    if (px >= 0 && px < w) {
                        row[px] = (uint16_t)XIAOZHI_CAMSTAMP_COLOR;
                    }
                }
            }
        }
    }
}

void xiaozhi_camstamp_draw(uint16_t *buf, int w, int h)
{
    if (buf == NULL || w <= 0 || h <= 0) {
        return;
    }

    /* 未授时（SNTP 还没成功）时 xiaozhi_time_format 返回 false：不画，避免出现 1970 */
    char text[CAMSTAMP_TEXT_MAX];
    if (!xiaozhi_time_format(text, sizeof(text))) {
        return;
    }

    const int len = (int)strlen(text);
    if (len <= 0) {
        return;
    }

    /* 字号自适应：整屏 320 宽用配置的 scale；放不下就逐级缩小（小叠加 80x100 会直接跳过） */
    int scale = XIAOZHI_CAMSTAMP_SCALE;
    while (scale > 1 && camstamp_text_width(len, scale) > w - 2 * XIAOZHI_CAMSTAMP_MARGIN_X) {
        scale--;
    }
    const int tw = camstamp_text_width(len, scale);
    const int th = CAMSTAMP_GLYPH_H * scale;
    if (tw > w - 2 * XIAOZHI_CAMSTAMP_MARGIN_X || th > h - 2 * XIAOZHI_CAMSTAMP_MARGIN_Y) {
        return; /* 画面太小，不画（例：右上角 80x100 叠加） */
    }

    int x = XIAOZHI_CAMSTAMP_MARGIN_X;
    int y = XIAOZHI_CAMSTAMP_MARGIN_Y;
#if XIAOZHI_CAMSTAMP_POS == XIAOZHI_CAMSTAMP_POS_TOP_RIGHT || \
    XIAOZHI_CAMSTAMP_POS == XIAOZHI_CAMSTAMP_POS_BOTTOM_RIGHT
    x = w - XIAOZHI_CAMSTAMP_MARGIN_X - tw;
#endif
#if XIAOZHI_CAMSTAMP_POS == XIAOZHI_CAMSTAMP_POS_BOTTOM_LEFT || \
    XIAOZHI_CAMSTAMP_POS == XIAOZHI_CAMSTAMP_POS_BOTTOM_RIGHT
    y = h - XIAOZHI_CAMSTAMP_MARGIN_Y - th;
#endif

#if XIAOZHI_CAMSTAMP_BOX
    /* 字底下压一层半透明黑底，浅色/杂乱画面上也看得清 */
    const int pad = scale;
    camstamp_darken_box(buf, w, h, x - pad, y - pad, x + tw + pad, y + th + pad);
#endif

    const int advance = (CAMSTAMP_GLYPH_W + XIAOZHI_CAMSTAMP_GAP) * scale;
    for (int i = 0; i < len; i++) {
        camstamp_draw_glyph(buf, w, h, x + i * advance, y, text[i], scale);
    }
}

#else /* XIAOZHI_CAMSTAMP_ENABLE == 0：空实现 */

void xiaozhi_camstamp_draw(uint16_t *buf, int w, int h)
{
    (void)buf;
    (void)w;
    (void)h;
}

#endif /* XIAOZHI_CAMSTAMP_ENABLE */
