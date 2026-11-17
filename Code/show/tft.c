/*
 * tft.c
 *
 * 创建日期：2024-05-24
 *
 * SPI TFT 底层驱动实现，负责控制器初始化、地址窗口设置、矩形填充和两套
 * 点阵字体绘制。传输使用阻塞式 HAL SPI，所有接口均应在主循环调用，不能在
 * ISR 中调用；坐标接口使用当前屏幕方向下的逻辑宽高。
 */

#include "tft.h"

#include <stddef.h>
#include <stdint.h>

#include "main.h"
#include "spi.h"

/* SPI 超时只作为故障兜底；6×8 连续字库覆盖空格至大写 Z。 */
#define TFT_SPI_TIMEOUT_MS 1000U
#define TFT_CHAR_FIRST     ' '
#define TFT_CHAR_LAST      'Z'

_Static_assert(FONT_6X8_GLYPH_COUNT ==
                   (uint32_t)(TFT_CHAR_LAST - TFT_CHAR_FIRST + 1),
               "6x8 font range and table size must match");

/* 四个控制引脚均为高/低电平直接驱动，宏参数使用 C 布尔语义。 */
#define LCD_RES(state) \
    HAL_GPIO_WritePin(LCD_RES_GPIO_Port, LCD_RES_Pin, (state) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define LCD_DC(state) \
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, (state) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define LCD_CS(state) \
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, (state) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define LCD_BLK(state) \
    HAL_GPIO_WritePin(LCD_BLK_GPIO_Port, LCD_BLK_Pin, (state) ? GPIO_PIN_SET : GPIO_PIN_RESET)

/** 控制器初始化表中的一条命令及其紧随的数据字节。 */
typedef struct
{
    uint8_t command;
    uint8_t data_length;
    const uint8_t *data;
} tft_init_command_t;

/*
 * 上电初始化命令按数组顺序发送。复合字面量中的数据具有静态存储期，指针在
 * 整个程序运行期间有效。0x36 依赖屏幕方向，退出休眠还需要独立延时，因此
 * 这两部分由 TFT_Init() 单独处理。
 */
static const tft_init_command_t init_commands[] = {
    {0xB1U, 3U, (const uint8_t[]){0x01U, 0x2CU, 0x2DU}},
    {0xB2U, 3U, (const uint8_t[]){0x01U, 0x2CU, 0x2DU}},
    {0xB3U, 6U, (const uint8_t[]){0x01U, 0x2CU, 0x2DU, 0x01U, 0x2CU, 0x2DU}},
    {0xB4U, 1U, (const uint8_t[]){0x07U}},
    {0xC0U, 3U, (const uint8_t[]){0xA2U, 0x02U, 0x84U}},
    {0xC1U, 1U, (const uint8_t[]){0xC5U}},
    {0xC2U, 2U, (const uint8_t[]){0x0AU, 0x00U}},
    {0xC3U, 2U, (const uint8_t[]){0x8AU, 0x2AU}},
    {0xC4U, 2U, (const uint8_t[]){0x8AU, 0xEEU}},
    {0xC5U, 1U, (const uint8_t[]){0x0EU}},
    {0xE0U,
     16U,
     (const uint8_t[]){0x0FU, 0x1AU, 0x0FU, 0x18U, 0x2FU, 0x28U, 0x20U, 0x22U,
                       0x1FU, 0x1BU, 0x23U, 0x37U, 0x00U, 0x07U, 0x02U, 0x10U}},
    {0xE1U,
     16U,
     (const uint8_t[]){0x0FU, 0x1BU, 0x0FU, 0x17U, 0x33U, 0x2CU, 0x29U, 0x2EU,
                       0x30U, 0x30U, 0x39U, 0x3FU, 0x00U, 0x07U, 0x03U, 0x10U}},
    {0x2AU, 4U, (const uint8_t[]){0x00U, 0x02U, 0x00U, 0x82U}},
    {0x2BU, 4U, (const uint8_t[]){0x00U, 0x03U, 0x00U, 0x83U}},
    {0xF0U, 1U, (const uint8_t[]){0x01U}},
    {0xF6U, 1U, (const uint8_t[]){0x00U}},
    {0x3AU, 1U, (const uint8_t[]){0x05U}},
    {0x21U, 0U, NULL},
    {0x29U, 0U, NULL},
};

/* MADCTL(0x36) 在四种安装方向下的参数，索引由 TFT_ORIENTATION 决定。 */
static const uint8_t orientation_parameters[] = {0xC8U, 0x08U, 0x68U, 0xA8U};

/* 字符绘制函数共享当前前景/背景色；仅由主循环访问，无需并发保护。 */
static uint16_t background_color = TFT_DEFAULT_BACKGROUND_COLOR;
static uint16_t text_color = TFT_DEFAULT_TEXT_COLOR;

/** 发送一个数据字节；调用方负责预先设置 DC 和 CS。 */
static void TFT_SendByte(uint8_t byte)
{
    (void)HAL_SPI_Transmit(&hspi1, &byte, 1U, TFT_SPI_TIMEOUT_MS);
}

/** 将 DC 拉低发送命令字节，随后恢复数据模式。 */
static void TFT_WriteCommand(uint8_t command)
{
    LCD_DC(false);
    TFT_SendByte(command);
    LCD_DC(true);
}

/**
 * @brief 设置下一次显存写入使用的闭区间矩形窗口。
 *
 * 控制器显存尺寸大于可见区域，不同旋转方向需要不同的行列偏移。函数依次发送
 * CASET(0x2A)、RASET(0x2B) 和 RAMWR(0x2C)，后续像素字节会自动递增地址。
 */
static void TFT_SetAddressWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    TFT_WriteCommand(0x2AU);
    TFT_SendByte(0x00U);
#if (TFT_ORIENTATION == 0U) || (TFT_ORIENTATION == 1U)
    TFT_SendByte((uint8_t)(x0 + 26U));
    TFT_SendByte(0x00U);
    TFT_SendByte((uint8_t)(x1 + 26U));
#else
    TFT_SendByte((uint8_t)(x0 + 1U));
    TFT_SendByte(0x00U);
    TFT_SendByte((uint8_t)(x1 + 1U));
#endif

    TFT_WriteCommand(0x2BU);
    TFT_SendByte(0x00U);
#if (TFT_ORIENTATION == 0U) || (TFT_ORIENTATION == 1U)
    TFT_SendByte((uint8_t)(y0 + 1U));
    TFT_SendByte(0x00U);
    TFT_SendByte((uint8_t)(y1 + 1U));
#else
    TFT_SendByte((uint8_t)(y0 + 26U));
    TFT_SendByte(0x00U);
    TFT_SendByte((uint8_t)(y1 + 26U));
#endif

    TFT_WriteCommand(0x2CU);
}

void TFT_Init(void)
{
    /* 硬件复位后等待控制器内部电源稳定，再退出休眠。 */
    LCD_RES(false);
    HAL_Delay(5U);
    LCD_RES(true);
    HAL_Delay(120U);

    LCD_CS(false);
    TFT_WriteCommand(0x11U);
    HAL_Delay(120U);

    /* 按表发送电源、帧率、伽马、像素格式和显示开启等控制器参数。 */
    for (size_t index = 0U;
         index < (sizeof(init_commands) / sizeof(init_commands[0]));
         index++)
    {
        TFT_WriteCommand(init_commands[index].command);
        for (uint8_t byte = 0U; byte < init_commands[index].data_length; byte++)
        {
            TFT_SendByte(init_commands[index].data[byte]);
        }
    }

    TFT_WriteCommand(0x36U);
    TFT_SendByte(orientation_parameters[TFT_ORIENTATION]);
    LCD_CS(true);

    LCD_BLK(true);
    TFT_Clear();
}

void TFT_SetBackgroundColor(rgb565_color_t color)
{
    background_color = (uint16_t)color;
}

void TFT_SetTextColor(rgb565_color_t color)
{
    text_color = (uint16_t)color;
}

void TFT_FillRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t color)
{
    uint8_t row_buffer[TFT_WIDTH * 2U];
    uint16_t width;

    if ((x0 > x1) || (y0 > y1) || (x0 >= TFT_WIDTH) || (y0 >= TFT_HEIGHT))
    {
        return;
    }

    if (x1 >= TFT_WIDTH)
    {
        x1 = TFT_WIDTH - 1U;
    }
    if (y1 >= TFT_HEIGHT)
    {
        y1 = TFT_HEIGHT - 1U;
    }

    width = (uint16_t)((uint16_t)x1 - (uint16_t)x0 + 1U);

    /* 预生成一整行 RGB565 大端像素，同一行缓冲区可复用于矩形中的每一行。 */
    for (uint16_t pixel = 0U; pixel < width; pixel++)
    {
        row_buffer[pixel * 2U] = (uint8_t)(color >> 8U);
        row_buffer[pixel * 2U + 1U] = (uint8_t)color;
    }

    LCD_CS(false);
    for (uint16_t row = y0; row <= y1; row++)
    {
        TFT_SetAddressWindow(x0, (uint8_t)row, x1, (uint8_t)row);
        (void)HAL_SPI_Transmit(&hspi1,
                               row_buffer,
                               (uint16_t)(width * 2U),
                               TFT_SPI_TIMEOUT_MS);
    }
    LCD_CS(true);
}

void TFT_DrawRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t color)
{
    if ((x0 > x1) || (y0 > y1))
    {
        return;
    }

    TFT_FillRect(x0, y0, x1, y0, color);
    TFT_FillRect(x0, y1, x1, y1, color);
    TFT_FillRect(x0, y0, x0, y1, color);
    TFT_FillRect(x1, y0, x1, y1, color);
}

void TFT_Clear(void)
{
    TFT_FillRect(0U,
                 0U,
                 TFT_WIDTH - 1U,
                 TFT_HEIGHT - 1U,
                 background_color);
}

void TFT_DrawChar(uint8_t x, uint8_t y, char character)
{
    uint8_t row_buffer[FONT_6X8_WIDTH * 2U];
    const uint8_t *font_data;

    if (((uint16_t)x + FONT_6X8_WIDTH > TFT_WIDTH) ||
        ((uint16_t)y + FONT_6X8_HEIGHT > TFT_HEIGHT))
    {
        return;
    }

    if ((character < TFT_CHAR_FIRST) || (character > TFT_CHAR_LAST))
    {
        character = TFT_CHAR_FIRST;
    }
    font_data = font_6x8[(uint8_t)(character - TFT_CHAR_FIRST)];

    LCD_CS(false);
    TFT_SetAddressWindow(x,
                         y,
                         (uint8_t)(x + FONT_6X8_WIDTH - 1U),
                         (uint8_t)(y + FONT_6X8_HEIGHT - 1U));

    /* 字库按列保存，每次重排一行即可，避免为整个字符申请像素缓冲区。 */
    for (uint8_t row = 0U; row < FONT_6X8_HEIGHT; row++)
    {
        for (uint8_t column = 0U; column < FONT_6X8_WIDTH; column++)
        {
            uint16_t color = ((font_data[column] & (1U << row)) != 0U)
                                 ? text_color
                                 : background_color;
            row_buffer[column * 2U] = (uint8_t)(color >> 8U);
            row_buffer[column * 2U + 1U] = (uint8_t)color;
        }

        (void)HAL_SPI_Transmit(&hspi1,
                               row_buffer,
                               sizeof(row_buffer),
                               TFT_SPI_TIMEOUT_MS);
    }

    LCD_CS(true);
}

void TFT_DrawString(uint8_t x, uint8_t y, const char *text)
{
    uint16_t cursor_x = x;

    if (text == NULL)
    {
        return;
    }

    while ((*text != '\0') && ((cursor_x + FONT_6X8_WIDTH) <= TFT_WIDTH))
    {
        TFT_DrawChar((uint8_t)cursor_x, y, *text);
        cursor_x += FONT_6X8_WIDTH;
        text++;
    }
}

void TFT_DrawInteger(uint8_t x, uint8_t y, int8_t value, bool show_sign)
{
    uint8_t absolute_value;
    uint8_t tens;
    uint8_t ones;
    uint8_t character_count = show_sign ? 3U : 2U;

    if (((uint16_t)x + (uint16_t)character_count * FONT_6X8_WIDTH > TFT_WIDTH) ||
        ((uint16_t)y + FONT_6X8_HEIGHT > TFT_HEIGHT))
    {
        return;
    }

    if (value > 99)
    {
        value = 99;
    }
    else if (value < -99)
    {
        value = -99;
    }

    if (show_sign)
    {
        TFT_DrawChar(x, y, (value < 0) ? '-' : '+');
        x = (uint8_t)(x + FONT_6X8_WIDTH);
    }

    absolute_value = (uint8_t)((value < 0) ? -value : value);
    tens = absolute_value / 10U;
    ones = absolute_value % 10U;

    TFT_DrawChar(x, y, (tens == 0U) ? ' ' : (char)('0' + tens));
    TFT_DrawChar((uint8_t)(x + FONT_6X8_WIDTH), y, (char)('0' + ones));
}

void TFT_DrawLargeGlyph(uint8_t x,
                        uint8_t y,
                        rgb565_color_t foreground,
                        rgb565_color_t background,
                        font_large_glyph_t glyph)
{
    uint8_t row_buffer[FONT_12X24_WIDTH * 2U];
    const uint8_t *font_data;

    if (((uint32_t)glyph >= FONT_LARGE_GLYPH_COUNT) ||
        ((uint16_t)x + FONT_12X24_WIDTH > TFT_WIDTH) ||
        ((uint16_t)y + FONT_12X24_HEIGHT > TFT_HEIGHT))
    {
        return;
    }

    font_data = font_12x24_selected[glyph];
    LCD_CS(false);
    TFT_SetAddressWindow(x,
                         y,
                         (uint8_t)(x + FONT_12X24_WIDTH - 1U),
                         (uint8_t)(y + FONT_12X24_HEIGHT - 1U));

    /* 每 8 行组成一个字节，低位对应该分组的上方像素。 */
    for (uint8_t row = 0U; row < FONT_12X24_HEIGHT; row++)
    {
        for (uint8_t column = 0U; column < FONT_12X24_WIDTH; column++)
        {
            uint8_t byte_index = (uint8_t)(((row >> 3U) * FONT_12X24_WIDTH) + column);
            uint8_t bit_index = row & 0x07U;
            uint16_t color = ((font_data[byte_index] & (1U << bit_index)) != 0U)
                                 ? (uint16_t)foreground
                                 : (uint16_t)background;

            row_buffer[column * 2U] = (uint8_t)(color >> 8U);
            row_buffer[column * 2U + 1U] = (uint8_t)color;
        }

        (void)HAL_SPI_Transmit(&hspi1,
                               row_buffer,
                               sizeof(row_buffer),
                               TFT_SPI_TIMEOUT_MS);
    }

    LCD_CS(true);
}

static void TFT_DrawLargeDigit(uint8_t x,
                               uint8_t y,
                               rgb565_color_t foreground,
                               rgb565_color_t background,
                               uint8_t digit)
{
    TFT_DrawLargeGlyph(x,
                       y,
                       foreground,
                       background,
                       (font_large_glyph_t)(FONT_LARGE_DIGIT_0 + digit));
}

static font_large_glyph_t TFT_MapLargeCharacter(char character)
{
    if ((character >= '0') && (character <= '9'))
    {
        return (font_large_glyph_t)(FONT_LARGE_DIGIT_0 + (character - '0'));
    }
    if (character == '-')
    {
        return FONT_LARGE_MINUS;
    }
    if (character == '.')
    {
        return FONT_LARGE_DECIMAL_POINT;
    }
    return FONT_LARGE_SPACE;
}

void TFT_DrawLargeText(uint8_t x,
                       uint8_t y,
                       rgb565_color_t foreground,
                       rgb565_color_t background,
                       const char *text,
                       uint8_t glyph_count)
{
    bool end_of_text = false;

    if ((text == NULL) || (glyph_count == 0U) ||
        ((uint16_t)x + (uint16_t)glyph_count * FONT_12X24_WIDTH > TFT_WIDTH) ||
        ((uint16_t)y + FONT_12X24_HEIGHT > TFT_HEIGHT))
    {
        return;
    }

    for (uint8_t index = 0U; index < glyph_count; index++)
    {
        char character = end_of_text ? ' ' : text[index];
        if (character == '\0')
        {
            end_of_text = true;
            character = ' ';
        }

        TFT_DrawLargeGlyph(x,
                           y,
                           foreground,
                           background,
                           TFT_MapLargeCharacter(character));
        x = (uint8_t)(x + FONT_12X24_WIDTH);
    }
}

void TFT_DrawIcon16(uint8_t x,
                    uint8_t y,
                    rgb565_color_t foreground,
                    rgb565_color_t background,
                    icon_16x16_frame_t frame)
{
    uint8_t row_buffer[ICON_16X16_WIDTH * 2U];
    const uint8_t *icon_data;

    if (((uint32_t)frame >= ICON_16X16_FRAME_COUNT) ||
        ((uint16_t)x + ICON_16X16_WIDTH > TFT_WIDTH) ||
        ((uint16_t)y + ICON_16X16_HEIGHT > TFT_HEIGHT))
    {
        return;
    }

    icon_data = icon_16x16_frames[frame];
    LCD_CS(false);
    TFT_SetAddressWindow(x,
                         y,
                         (uint8_t)(x + ICON_16X16_WIDTH - 1U),
                         (uint8_t)(y + ICON_16X16_HEIGHT - 1U));

    for (uint8_t row = 0U; row < ICON_16X16_HEIGHT; row++)
    {
        for (uint8_t column = 0U; column < ICON_16X16_WIDTH; column++)
        {
            uint8_t byte_index =
                (uint8_t)(((row >> 3U) * ICON_16X16_WIDTH) + column);
            uint8_t bit_index = row & 0x07U;
            uint16_t color = ((icon_data[byte_index] & (1U << bit_index)) != 0U)
                                 ? (uint16_t)foreground
                                 : (uint16_t)background;

            row_buffer[column * 2U] = (uint8_t)(color >> 8U);
            row_buffer[column * 2U + 1U] = (uint8_t)color;
        }

        (void)HAL_SPI_Transmit(&hspi1,
                               row_buffer,
                               sizeof(row_buffer),
                               TFT_SPI_TIMEOUT_MS);
    }

    LCD_CS(true);
}

void TFT_DrawLargeUnsigned(uint8_t x,
                           uint8_t y,
                           rgb565_color_t foreground,
                           rgb565_color_t background,
                           uint16_t value,
                           uint8_t digits,
                           bool leading_zeroes)
{
    uint32_t divisor = 1U;
    uint32_t maximum;
    bool significant = leading_zeroes;

    if ((digits == 0U) || (digits > 5U) ||
        ((uint16_t)x + (uint16_t)digits * FONT_12X24_WIDTH > TFT_WIDTH))
    {
        return;
    }

    for (uint8_t index = 1U; index < digits; index++)
    {
        divisor *= 10U;
    }
    maximum = divisor * 10U - 1U;
    if ((uint32_t)value > maximum)
    {
        value = (uint16_t)maximum;
    }

    for (uint8_t index = 0U; index < digits; index++)
    {
        uint8_t digit = (uint8_t)((uint32_t)value / divisor);
        value = (uint16_t)((uint32_t)value % divisor);

        if ((digit != 0U) || (index == (uint8_t)(digits - 1U)))
        {
            significant = true;
        }

        if (significant)
        {
            TFT_DrawLargeDigit(x, y, foreground, background, digit);
        }
        else
        {
            TFT_DrawLargeGlyph(x,
                               y,
                               foreground,
                               background,
                               FONT_LARGE_SPACE);
        }

        x = (uint8_t)(x + FONT_12X24_WIDTH);
        if (divisor > 1U)
        {
            divisor /= 10U;
        }
    }
}

