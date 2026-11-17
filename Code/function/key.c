/*
 * key.c
 *
 * TIM3 正交编码器和 PC15 低有效按键的输入适配。模块由 TIM14 每 10 ms 扫描
 * 一次，只执行 GPIO/定时器寄存器访问；扫描结果作为紧凑事件交给控制层处理。
 */

#include "key.h"

#include <stdbool.h>
#include <stdint.h>

#include "main.h"

/* TIM3 为 X4 计数，每四个计数对应编码器一个机械刻度。 */
#define ENCODER_COUNTS_PER_DETENT 4L
#define BUTTON_DEBOUNCE_MS        30U
#define BUTTON_DEBOUNCE_TICKS     (BUTTON_DEBOUNCE_MS / KEY_SCAN_PERIOD_MS)

_Static_assert((KEY_ENCODER_DIRECTION == KEY_ENCODER_DIRECTION_FORWARD) ||
               (KEY_ENCODER_DIRECTION == KEY_ENCODER_DIRECTION_REVERSED),
               "encoder direction must be forward or reversed");
_Static_assert((BUTTON_DEBOUNCE_MS % KEY_SCAN_PERIOD_MS) == 0U,
               "button debounce must align with scan period");
_Static_assert(BUTTON_DEBOUNCE_TICKS > 0U,
               "button debounce must cover at least one scan");

/*
 * 编码器余数保留不足一格的 X4 计数。按键候选状态必须连续保持指定周期，
 * 才能替换稳定状态；一次稳定按下并释放只产生一个短按事件。
 */
static uint16_t encoder_last_count;
static int32_t encoder_remainder;
static bool button_candidate_pressed;
static bool button_stable_pressed;
static uint8_t button_candidate_ticks;

void Key_Init(void)
{
    LL_TIM_SetCounter(TIM3, 0U);
    LL_TIM_ClearFlag_UPDATE(TIM3);
    LL_TIM_EnableCounter(TIM3);

    encoder_last_count = (uint16_t)LL_TIM_GetCounter(TIM3);
    encoder_remainder = 0L;
    button_candidate_pressed = false;
    button_stable_pressed = false;
    button_candidate_ticks = 0U;
}

key_input_t Key_Scan(void)
{
    key_input_t input = {0};
    uint16_t current_count = (uint16_t)LL_TIM_GetCounter(TIM3);
    /* 转成 int16_t 后，16 位计数器在相邻扫描间的一次回绕可自然得到有符号差值。 */
    int16_t raw_delta = (int16_t)(current_count - encoder_last_count);
    int32_t available_steps;
    bool raw_button_pressed;

    encoder_last_count = current_count;
    encoder_remainder += (int32_t)raw_delta * KEY_ENCODER_DIRECTION;
    available_steps = encoder_remainder / ENCODER_COUNTS_PER_DETENT;

    /* 事件字段为 int8_t，极端快速旋转时饱和输出，未上报的计数继续留在余数中。 */
    if (available_steps > INT8_MAX)
    {
        available_steps = INT8_MAX;
    }
    else if (available_steps < INT8_MIN)
    {
        available_steps = INT8_MIN;
    }

    input.encoder_steps = (int8_t)available_steps;
    encoder_remainder -= available_steps * ENCODER_COUNTS_PER_DETENT;

    raw_button_pressed =
        HAL_GPIO_ReadPin(ENCODER_KEY_GPIO_Port, ENCODER_KEY_Pin) == GPIO_PIN_RESET;

    /* 原始电平变化后重新计时；候选状态不变时才累计连续稳定周期。 */
    if (raw_button_pressed != button_candidate_pressed)
    {
        button_candidate_pressed = raw_button_pressed;
        button_candidate_ticks = 1U;
    }
    else if (button_candidate_ticks < BUTTON_DEBOUNCE_TICKS)
    {
        button_candidate_ticks++;
    }

    if ((button_candidate_ticks >= BUTTON_DEBOUNCE_TICKS) &&
        (button_stable_pressed != button_candidate_pressed))
    {
        button_stable_pressed = button_candidate_pressed;
        if (!button_stable_pressed)
        {
            input.button_short_pressed = true;
        }
    }

    return input;
}
