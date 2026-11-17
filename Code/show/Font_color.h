/*
 * Font_color.h
 *
 * TFT 使用的 RGB565 颜色、点阵字库和动画帧声明。所有数组均为只读常量，
 * 由链接器放入 Flash，具体位序和绘制方式由 tft.c 解释。
 */

#ifndef FONT_COLOR_H
#define FONT_COLOR_H

#include <stdint.h>

/* 6×8 字模每列一个字节；12×24 和 16×16 数据每列按 8 行分块保存。 */
#define FONT_6X8_WIDTH             6U
#define FONT_6X8_HEIGHT            8U
#define FONT_6X8_GLYPH_COUNT       59U
#define FONT_12X24_WIDTH           12U
#define FONT_12X24_HEIGHT          24U
#define FONT_12X24_BYTES_PER_GLYPH 36U
#define ICON_16X16_WIDTH            16U
#define ICON_16X16_HEIGHT           16U
#define ICON_16X16_BYTES_PER_FRAME  32U

/** TFT 使用的 16 位 RGB565 常用颜色。 */
typedef enum
{
    RGB565_WHITE = 0xFFFF,
    RGB565_BLACK = 0x0000,
    RGB565_BLUE = 0x001F,
    RGB565_PURPLE = 0xF81F,
    RGB565_PINK = 0xFE19,
    RGB565_RED = 0xF800,
    RGB565_MAGENTA = 0xF81F,
    RGB565_GREEN = 0x07E0,
    RGB565_CYAN = 0x07FF,
    RGB565_YELLOW = 0xFFE0,
    RGB565_ORANGE = 0xFD20,
    RGB565_BROWN = 0xBC40,
    RGB565_GRAY = 0x8430,
    RGB565_MUTED = 0x7BEF,
    RGB565_DARK_NAVY = 0x0021,
    RGB565_CARD = 0x10A2,
    RGB565_BLANCHED_ALMOND = 0xFF59
} rgb565_color_t;

/** 12×24 精简字库中的固定字形索引。 */
typedef enum
{
    FONT_LARGE_SPACE = 0,
    FONT_LARGE_MINUS,
    FONT_LARGE_DECIMAL_POINT,
    FONT_LARGE_DIGIT_0,
    FONT_LARGE_DIGIT_1,
    FONT_LARGE_DIGIT_2,
    FONT_LARGE_DIGIT_3,
    FONT_LARGE_DIGIT_4,
    FONT_LARGE_DIGIT_5,
    FONT_LARGE_DIGIT_6,
    FONT_LARGE_DIGIT_7,
    FONT_LARGE_DIGIT_8,
    FONT_LARGE_DIGIT_9,
    FONT_LARGE_GLYPH_COUNT
} font_large_glyph_t;

/** FAN/LED 状态区使用的 16×16 单色动画帧索引。 */
typedef enum
{
    ICON_FAN_FRAME_0 = 0,
    ICON_FAN_FRAME_1,
    ICON_FAN_FRAME_2,
    ICON_FAN_FRAME_3,
    ICON_LED_OFF,
    ICON_LED_DIM,
    ICON_LED_BRIGHT,
    ICON_16X16_FRAME_COUNT
} icon_16x16_frame_t;

/** 6×8 连续 ASCII 字模，索引依次对应空格（0x20）至大写 Z（0x5A）。 */
extern const uint8_t font_6x8[FONT_6X8_GLYPH_COUNT][FONT_6X8_WIDTH];

/** 12×24 精简字库，仅包含空格、负号、小数点和数字 0..9。 */
extern const uint8_t
    font_12x24_selected[FONT_LARGE_GLYPH_COUNT][FONT_12X24_BYTES_PER_GLYPH];

/** 16×16 动画图标，每列由上下两个低位在上的 8 位数据块组成。 */
extern const uint8_t
    icon_16x16_frames[ICON_16X16_FRAME_COUNT][ICON_16X16_BYTES_PER_FRAME];

#endif /* FONT_COLOR_H */
