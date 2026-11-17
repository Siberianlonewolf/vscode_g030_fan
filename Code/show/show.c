/*
 * show.c
 *
 * 160×80 风扇控制仪表界面。电压、电流、功率和转速使用 12×24 精简字库，
 * FAN/LED 状态使用 16×16 动画帧。模块保存上一帧状态，仅重绘发生变化的
 * 区域；所有阻塞式 SPI 绘制均在主循环执行。
 */

#include "show.h"

#include <stdbool.h>
#include <stdint.h>

#include "Font_color.h"
#include "control.h"
#include "tft.h"

/* 界面颜色按语义集中定义，避免各绘制函数直接散布 RGB565 数值。 */
#define DASHBOARD_BACKGROUND RGB565_DARK_NAVY
#define DASHBOARD_CARD       RGB565_CARD
#define DASHBOARD_ACCENT     RGB565_CYAN
#define DASHBOARD_GOOD       RGB565_GREEN
#define DASHBOARD_WARNING    RGB565_ORANGE
#define DASHBOARD_BAD        RGB565_RED
#define DASHBOARD_MUTED      RGB565_MUTED

/* 风扇共有四个旋转帧，停转后继续播放三帧模拟惯性。 */
#define FAN_FRAME_COUNT      4U
#define FAN_STOP_FRAME_COUNT 3U

/*
 * displayed_state 是最后一次提交到屏幕的快照，用于判断各区域是否需要重绘。
 * 动画变量独立保存，因为动画即使业务状态不变也要随 Show_Update() 推进。
 */
static control_state_t displayed_state;
static bool display_cache_valid;
static uint8_t fan_animation_frame;
static uint8_t fan_stop_frames_remaining;
static bool fan_was_moving;
static uint8_t led_brightness;
static bool led_was_enabled;
static bool led_breathe_up;

static void Show_DrawText(uint8_t x,
                          uint8_t y,
                          const char *text,
                          rgb565_color_t foreground,
                          rgb565_color_t background)
{
    TFT_SetTextColor(foreground);
    TFT_SetBackgroundColor(background);
    TFT_DrawString(x, y, text);
}

static uint32_t Show_AbsoluteS32(int32_t value)
{
    if (value >= 0L)
    {
        return (uint32_t)value;
    }
    return (uint32_t)(-(value + 1L)) + 1UL;
}

static void Show_WriteRightAligned(char buffer[5], uint32_t value, uint8_t digits)
{
    for (uint8_t index = 0U; index < 4U; index++)
    {
        buffer[index] = ' ';
    }
    buffer[4] = '\0';

    if (digits > 4U)
    {
        digits = 4U;
    }

    for (uint8_t index = 0U; index < digits; index++)
    {
        buffer[3U - index] = (char)('0' + (value % 10UL));
        value /= 10UL;
    }
}

/*
 * 把以千分之一基本单位表示的有符号工程量格式化为固定四个大字形：
 *   正数 <9.95  显示一位整数、两位小数；
 *   正数 <99.95 显示两位整数、一位小数；更大数值显示四位整数；
 *   负数为符号保留一位，并按量级减少小数位。
 * 各分界点先按目标精度四舍五入，超出可显示范围时饱和。
 */
static void Show_FormatMetric4(char buffer[5], int32_t milli_value)
{
    uint32_t absolute_value = Show_AbsoluteS32(milli_value);
    bool negative = milli_value < 0L;

    if (!negative && (absolute_value < 9950UL))
    {
        uint32_t centi = (absolute_value + 5UL) / 10UL;
        buffer[0] = (char)('0' + (centi / 100UL));
        buffer[1] = '.';
        buffer[2] = (char)('0' + ((centi / 10UL) % 10UL));
        buffer[3] = (char)('0' + (centi % 10UL));
    }
    else if (!negative && (absolute_value < 99950UL))
    {
        uint32_t deci = (absolute_value + 50UL) / 100UL;
        buffer[0] = (char)('0' + (deci / 100UL));
        buffer[1] = (char)('0' + ((deci / 10UL) % 10UL));
        buffer[2] = '.';
        buffer[3] = (char)('0' + (deci % 10UL));
    }
    else if (!negative)
    {
        uint32_t whole = (absolute_value + 500UL) / 1000UL;
        if (whole > 9999UL)
        {
            whole = 9999UL;
        }
        Show_WriteRightAligned(buffer,
                               whole,
                               (whole >= 1000UL) ? 4U :
                               (whole >= 100UL) ? 3U :
                               (whole >= 10UL) ? 2U : 1U);
    }
    else if (absolute_value < 995UL)
    {
        uint32_t centi = (absolute_value + 5UL) / 10UL;
        buffer[0] = '-';
        buffer[1] = '.';
        buffer[2] = (char)('0' + ((centi / 10UL) % 10UL));
        buffer[3] = (char)('0' + (centi % 10UL));
    }
    else if (absolute_value < 9950UL)
    {
        uint32_t deci = (absolute_value + 50UL) / 100UL;
        buffer[0] = '-';
        buffer[1] = (char)('0' + (deci / 10UL));
        buffer[2] = '.';
        buffer[3] = (char)('0' + (deci % 10UL));
    }
    else
    {
        uint32_t whole = (absolute_value + 500UL) / 1000UL;
        if (whole > 999UL)
        {
            whole = 999UL;
        }
        buffer[0] = '-';
        buffer[1] = (whole >= 100UL) ? (char)('0' + (whole / 100UL)) : ' ';
        buffer[2] = (whole >= 10UL) ? (char)('0' + ((whole / 10UL) % 10UL)) : ' ';
        buffer[3] = (char)('0' + (whole % 10UL));
    }

    buffer[4] = '\0';
}

static void Show_FormatPercent(char buffer[8], char prefix, uint8_t percent)
{
    buffer[0] = prefix;
    buffer[1] = ' ';
    buffer[2] = (percent >= 100U) ? '1' : ' ';
    buffer[3] = (percent >= 10U) ? (char)('0' + ((percent / 10U) % 10U)) : ' ';
    buffer[4] = (char)('0' + (percent % 10U));
    buffer[5] = '%';
    buffer[6] = ' ';
    buffer[7] = '\0';
}

/*
 * 顶栏诊断按“离线 > 配置异常 > 兼容模式 > 身份异常 > 正常”的顺序选择，
 * 保证影响采样可用性的状态优先显示。
 */
static void Show_DrawInaStatus(const control_state_t *state)
{
    const char *text;
    rgb565_color_t color;

    if (!state->ina226_online)
    {
        text = "INA ERR";
        color = DASHBOARD_BAD;
    }
    else if (!state->ina226_configuration_valid)
    {
        text = "INA CFG";
        color = DASHBOARD_WARNING;
    }
    else if (state->ina219_compatible)
    {
        text = "INA219";
        color = DASHBOARD_ACCENT;
    }
    else if (!state->ina226_identity_valid)
    {
        text = "INA ID";
        color = DASHBOARD_WARNING;
    }
    else
    {
        text = "INA OK";
        color = DASHBOARD_GOOD;
    }

    TFT_FillRect(109U, 1U, 158U, 8U, DASHBOARD_CARD);
    Show_DrawText(112U, 1U, text, color, DASHBOARD_CARD);
}

/* 三张测量卡共用四字形格式；无完整采样时统一覆盖为占位符。 */
static void Show_DrawMeasurements(const control_state_t *state, bool measurements_ok)
{
    char text[5];

    if (!measurements_ok)
    {
        TFT_DrawLargeText(3U, 20U, DASHBOARD_MUTED, DASHBOARD_CARD, "----", 4U);
        TFT_DrawLargeText(56U, 20U, DASHBOARD_MUTED, DASHBOARD_CARD, "----", 4U);
        TFT_DrawLargeText(110U, 20U, DASHBOARD_MUTED, DASHBOARD_CARD, "----", 4U);
        return;
    }

    Show_FormatMetric4(text, (int32_t)state->input_voltage_mv);
    TFT_DrawLargeText(3U, 20U, RGB565_CYAN, DASHBOARD_CARD, text, 4U);
    Show_FormatMetric4(text, state->current_ma);
    TFT_DrawLargeText(56U, 20U, RGB565_GREEN, DASHBOARD_CARD, text, 4U);
    Show_FormatMetric4(text, state->power_mw);
    TFT_DrawLargeText(110U, 20U, RGB565_ORANGE, DASHBOARD_CARD, text, 4U);
}

static void Show_DrawRpm(const control_state_t *state)
{
    TFT_DrawLargeUnsigned(2U,
                          55U,
                          state->tach_valid ? RGB565_WHITE : DASHBOARD_MUTED,
                          DASHBOARD_CARD,
                          state->actual_rpm,
                          5U,
                          false);
}

static void Show_DrawFanDetails(const control_state_t *state)
{
    char target_text[8];
    char pwm_text[8];
    const char *status_text;
    rgb565_color_t status_color;

    Show_FormatPercent(target_text, 'T', state->target_duty_percent);
    Show_FormatPercent(pwm_text, 'P', state->fan_pwm_percent);

    if (!state->fan_running)
    {
        status_text = "STOP";
        status_color = DASHBOARD_MUTED;
    }
    else
    {
        status_text = "RUN";
        status_color = DASHBOARD_GOOD;
    }

    TFT_FillRect(64U, 46U, 107U, 79U, DASHBOARD_CARD);
    Show_DrawText(65U, 47U, target_text, DASHBOARD_ACCENT, DASHBOARD_CARD);
    Show_DrawText(65U, 57U, pwm_text, DASHBOARD_WARNING, DASHBOARD_CARD);
    Show_DrawText(65U, 69U, status_text, status_color, DASHBOARD_CARD);
}

/*
 * 有效测速优先决定风扇是否转动；没有测速信号时依据实际 PWM 推断运动。
 * LED 图标按三档亮度呼吸。
 */
static void Show_DrawAnimations(const control_state_t *state)
{
    bool fan_moving = (state->tach_valid && (state->actual_rpm > 0U)) ||
                      (!state->tach_valid && state->fan_running);
    icon_16x16_frame_t fan_icon;
    icon_16x16_frame_t led_icon;
    rgb565_color_t fan_color;
    rgb565_color_t led_color;

    if (fan_moving)
    {
        fan_animation_frame = (uint8_t)((fan_animation_frame + 1U) % FAN_FRAME_COUNT);
        fan_stop_frames_remaining = 0U;
        fan_color = DASHBOARD_GOOD;
    }
    else
    {
        if (fan_was_moving)
        {
            fan_stop_frames_remaining = FAN_STOP_FRAME_COUNT;
        }

        if (fan_stop_frames_remaining > 0U)
        {
            fan_animation_frame = (uint8_t)((fan_animation_frame + 1U) % FAN_FRAME_COUNT);
            fan_stop_frames_remaining--;
            fan_color = DASHBOARD_WARNING;
        }
        else
        {
            fan_animation_frame = 0U;
            fan_color = DASHBOARD_MUTED;
        }
    }
    fan_icon = (icon_16x16_frame_t)(ICON_FAN_FRAME_0 + fan_animation_frame);
    fan_was_moving = fan_moving;

    if (state->led_enabled)
    {
        if (!led_was_enabled)
        {
            led_brightness = 0U;
            led_breathe_up = true;
        }
        else if (led_breathe_up)
        {
            if (led_brightness < 2U)
            {
                led_brightness++;
            }
            else
            {
                led_breathe_up = false;
            }
        }
        else if (led_brightness > 1U)
        {
            led_brightness--;
        }
        else
        {
            led_breathe_up = true;
        }
    }
    else if (led_was_enabled)
    {
        if (led_brightness > 0U)
        {
            led_brightness--;
        }
    }
    else
    {
        led_brightness = 0U;
    }

    led_icon = (led_brightness == 0U) ? ICON_LED_OFF
             : (led_brightness == 1U) ? ICON_LED_DIM
                                      : ICON_LED_BRIGHT;
    led_color = state->led_enabled ? RGB565_YELLOW : DASHBOARD_MUTED;
    led_was_enabled = state->led_enabled;

    TFT_DrawIcon16(113U, 59U, fan_color, DASHBOARD_CARD, fan_icon);
    TFT_DrawIcon16(139U, 59U, led_color, DASHBOARD_CARD, led_icon);
}

void Show_Init(void)
{
    TFT_Init();
    TFT_SetBackgroundColor(DASHBOARD_BACKGROUND);
    TFT_Clear();

    /*
     * 固定布局：0..9 顶栏，11..44 三张测量卡，46..79 为转速、目标/实际
     * 占空比、运行状态和 FAN/LED 动画。页面不再保留底部菜单单元格。
     */

    TFT_FillRect(0U, 0U, 159U, 9U, DASHBOARD_CARD);
    Show_DrawText(3U, 1U, "SMART FAN", RGB565_WHITE, DASHBOARD_CARD);

    TFT_FillRect(1U, 11U, 51U, 44U, DASHBOARD_CARD);
    TFT_FillRect(54U, 11U, 105U, 44U, DASHBOARD_CARD);
    TFT_FillRect(108U, 11U, 158U, 44U, DASHBOARD_CARD);
    Show_DrawText(4U, 12U, "VOLT V", RGB565_CYAN, DASHBOARD_CARD);
    Show_DrawText(57U, 12U, "CURR A", RGB565_GREEN, DASHBOARD_CARD);
    Show_DrawText(111U, 12U, "POWER W", RGB565_ORANGE, DASHBOARD_CARD);

    TFT_FillRect(1U, 46U, 62U, 79U, DASHBOARD_CARD);
    TFT_FillRect(64U, 46U, 107U, 79U, DASHBOARD_CARD);
    TFT_FillRect(109U, 46U, 133U, 79U, DASHBOARD_CARD);
    TFT_FillRect(135U, 46U, 158U, 79U, DASHBOARD_CARD);
    Show_DrawText(4U, 47U, "RPM", DASHBOARD_MUTED, DASHBOARD_CARD);
    Show_DrawText(112U, 47U, "FAN", DASHBOARD_MUTED, DASHBOARD_CARD);
    Show_DrawText(138U, 47U, "LED", DASHBOARD_MUTED, DASHBOARD_CARD);

    displayed_state = (control_state_t){0};
    display_cache_valid = false;
    fan_animation_frame = 0U;
    fan_stop_frames_remaining = 0U;
    fan_was_moving = false;
    led_brightness = 0U;
    led_was_enabled = false;
    led_breathe_up = true;
}

void Show_Update(void)
{
    const control_state_t *state = Control_GetState();
    bool measurements_ok = state->ina226_online &&
                           state->measurements_valid;
    bool displayed_measurements_ok = displayed_state.ina226_online &&
                                     displayed_state.measurements_valid;

    /*
     * 每个静态区域只比较真正影响其像素的字段。首次绘制忽略缓存，确保所有
     * 动态区域都有确定内容；测量有效性变化时即使数值相同也必须重绘。
     */

    if (!display_cache_valid ||
        (state->ina226_online != displayed_state.ina226_online) ||
        (state->ina219_compatible != displayed_state.ina219_compatible) ||
        (state->ina226_identity_valid != displayed_state.ina226_identity_valid) ||
        (state->ina226_configuration_valid != displayed_state.ina226_configuration_valid))
    {
        Show_DrawInaStatus(state);
    }

    if (!display_cache_valid ||
        (measurements_ok != displayed_measurements_ok) ||
        (measurements_ok &&
         ((state->input_voltage_mv != displayed_state.input_voltage_mv) ||
          (state->current_ma != displayed_state.current_ma) ||
          (state->power_mw != displayed_state.power_mw))))
    {
        Show_DrawMeasurements(state, measurements_ok);
    }

    if (!display_cache_valid ||
        (state->tach_valid != displayed_state.tach_valid) ||
        (state->actual_rpm != displayed_state.actual_rpm))
    {
        Show_DrawRpm(state);
    }

    if (!display_cache_valid ||
        (state->target_duty_percent != displayed_state.target_duty_percent) ||
        (state->fan_pwm_percent != displayed_state.fan_pwm_percent) ||
        (state->fan_running != displayed_state.fan_running))
    {
        Show_DrawFanDetails(state);
    }

    /* 动画按调用次数推进，不参与静态区域的缓存判断。 */
    Show_DrawAnimations(state);

    displayed_state = *state;
    display_cache_valid = true;

    /* 恢复底层驱动的默认文字颜色，避免本帧局部配色泄漏到其他调用方。 */
    TFT_SetBackgroundColor(DASHBOARD_BACKGROUND);
    TFT_SetTextColor(TFT_DEFAULT_TEXT_COLOR);
}
