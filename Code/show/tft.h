/*
 * tft.h
 *
 * SPI TFT 绘图接口，定义屏幕方向、逻辑尺寸、默认颜色和基本图形/文字原语。
 * 实现采用阻塞式 SPI 且共享当前文字颜色，只供主循环使用，不保证 ISR 或
 * 多线程重入安全。
 */

#ifndef TFT_H
#define TFT_H

#include <stdbool.h>
#include <stdint.h>

#include "Font_color.h"

/*
 * 屏幕方向索引范围为 0..3。方向 0/1 使用 80×160 逻辑坐标，方向 2/3
 * 使用 160×80；当前界面按横屏方向 2 布局。
 */
#define TFT_ORIENTATION 3U

#if (TFT_ORIENTATION == 0U) || (TFT_ORIENTATION == 1U)
#define TFT_WIDTH  80U
#define TFT_HEIGHT 160U
#else
#define TFT_WIDTH  160U
#define TFT_HEIGHT 80U
#endif

#define TFT_DEFAULT_BACKGROUND_COLOR RGB565_DARK_NAVY
#define TFT_DEFAULT_TEXT_COLOR       RGB565_WHITE

/** 复位并初始化 TFT 控制器，打开背光后使用默认背景色清屏。 */
void TFT_Init(void);

/** @brief 设置后续 6×8 字符绘制使用的背景色。 */
void TFT_SetBackgroundColor(rgb565_color_t color);

/** @brief 设置后续 6×8 字符绘制使用的前景色。 */
void TFT_SetTextColor(rgb565_color_t color);

/**
 * @brief 使用指定 RGB565 颜色填充闭区间矩形。
 *
 * 右边界和下边界超出屏幕时会被裁剪；起点越界或坐标次序无效时不绘制。
 */
void TFT_FillRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t color);

/** @brief 使用当前背景色填充整个逻辑屏幕。 */
void TFT_Clear(void);

/**
 * @brief 使用 6×8 点阵绘制一个字符。
 *
 * 字库只覆盖空格至大写 Z，其他字符按空格处理；完整字形越界时不绘制。
 */
void TFT_DrawChar(uint8_t x, uint8_t y, char character);

/**
 * @brief 使用 6×8 点阵绘制以空字符结尾的字符串。
 *
 * 绘制到屏幕右边界即停止，纵向无法容纳完整字形时对应字符不绘制。
 */
void TFT_DrawString(uint8_t x, uint8_t y, const char *text);

/**
 * @brief 使用 6×8 点阵绘制十进制整数。
 *
 * 数值限制在 -99..99；show_sign 为 true 时固定绘制符号和两位数字。
 */
void TFT_DrawInteger(uint8_t x, uint8_t y, int8_t value, bool show_sign);

/**
 * @brief 使用 12×24 精简字库绘制一个字形。
 *
 * 字形索引无效或完整字形超出屏幕时不绘制。
 */
void TFT_DrawLargeGlyph(uint8_t x,
                        uint8_t y,
                        rgb565_color_t foreground,
                        rgb565_color_t background,
                        font_large_glyph_t glyph);

/** @brief 绘制一像素宽的闭区间矩形边框，越界部分按填充接口规则裁剪。 */
void TFT_DrawRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t color);

/**
 * @brief 使用 12×24 精简字库绘制固定数量的字形。
 *
 * 仅空格、负号、小数点和数字可见；字符串不足 glyph_count 时以空格补齐，
 * 整个区域不能完整放入屏幕时不绘制。
 */
void TFT_DrawLargeText(uint8_t x,
                       uint8_t y,
                       rgb565_color_t foreground,
                       rgb565_color_t background,
                       const char *text,
                       uint8_t glyph_count);

/** @brief 使用指定颜色绘制一帧 16×16 FAN/LED 单色图标。 */
void TFT_DrawIcon16(uint8_t x,
                    uint8_t y,
                    rgb565_color_t foreground,
                    rgb565_color_t background,
                    icon_16x16_frame_t frame);

/**
 * @brief 使用 1..5 个 12×24 字形绘制无符号整数。
 *
 * 数值超出指定数字位数时饱和到全部为 9；leading_zeroes 控制前导零，
 * 未显示的高位使用空格覆盖。
 */
void TFT_DrawLargeUnsigned(uint8_t x,
                           uint8_t y,
                           rgb565_color_t foreground,
                           rgb565_color_t background,
                           uint16_t value,
                           uint8_t digits,
                           bool leading_zeroes);

#endif /* TFT_H */
