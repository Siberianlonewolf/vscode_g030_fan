/*
 * adc_input.c
 *
 * ADC1_IN5 电位器采样与占空比换算。所有 HAL ADC 调用都在主循环执行，避免
 * 在 TIM14 中断中引入等待；采样失败时保持最近一次有效目标，防止风扇突变。
 */

#include "adc_input.h"

#include <stdbool.h>
#include <stdint.h>

#include "adc.h"
#include "fan.h"

#define ADC_INPUT_MAX_RAW            4095UL
#define ADC_INPUT_FILTER_DENOMINATOR (1UL << ADC_INPUT_FILTER_SHIFT)

_Static_assert(ADC_INPUT_DEADBAND_RAW < ADC_INPUT_MAX_RAW,
               "ADC deadband must stay below full scale");
_Static_assert((ADC_INPUT_FILTER_SHIFT >= 1U) &&
               (ADC_INPUT_FILTER_SHIFT <= 8U),
               "ADC filter shift must be between 1 and 8");
_Static_assert(ADC_INPUT_POLL_TIMEOUT_MS > 0U,
               "ADC polling timeout must be positive");

static uint32_t filtered_raw;
static uint8_t last_duty_percent;
static bool filter_valid;
static bool adc_ready;

void AdcInput_Init(void)
{
    filtered_raw = 0U;
    last_duty_percent = 0U;
    filter_valid = false;
    adc_ready = HAL_ADCEx_Calibration_Start(&hadc1) == HAL_OK;
}

uint8_t AdcInput_ReadDutyPercent(void)
{
    uint32_t raw;
    uint32_t adjusted_raw;
    uint32_t active_range;
    uint32_t duty_percent;

    if (!adc_ready)
    {
        return 0U;
    }

    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
        return last_duty_percent;
    }

    if (HAL_ADC_PollForConversion(&hadc1, ADC_INPUT_POLL_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&hadc1);
        return last_duty_percent;
    }

    raw = HAL_ADC_GetValue(&hadc1);
    (void)HAL_ADC_Stop(&hadc1);
    if (raw > ADC_INPUT_MAX_RAW)
    {
        raw = ADC_INPUT_MAX_RAW;
    }

    if (!filter_valid)
    {
        filtered_raw = raw;
        filter_valid = true;
    }
    else
    {
        filtered_raw =
            (filtered_raw * (ADC_INPUT_FILTER_DENOMINATOR - 1UL) +
             raw + (ADC_INPUT_FILTER_DENOMINATOR / 2UL)) /
            ADC_INPUT_FILTER_DENOMINATOR;
    }

    if (filtered_raw <= ADC_INPUT_DEADBAND_RAW)
    {
        last_duty_percent = 0U;
        return last_duty_percent;
    }

    adjusted_raw = filtered_raw - ADC_INPUT_DEADBAND_RAW;
    active_range = ADC_INPUT_MAX_RAW - ADC_INPUT_DEADBAND_RAW;
    duty_percent =
        (adjusted_raw * FAN_MAX_TARGET_DUTY_PERCENT + active_range / 2UL) /
        active_range;
    if (duty_percent > FAN_MAX_TARGET_DUTY_PERCENT)
    {
        duty_percent = FAN_MAX_TARGET_DUTY_PERCENT;
    }

    last_duty_percent = (uint8_t)duty_percent;
    return last_duty_percent;
}
