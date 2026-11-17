/*
 * control.c
 *
 * 应用控制协调器。按键只切换 LED；编译期选择旋转编码器或 PA5 ADC 电位器
 * 设置风扇开环占空比。TIM14 中断只累积输入和 10 ms 节拍，ADC/I²C/SPI 与
 * 风扇状态汇总均在主循环执行，避免阻塞外设访问进入 ISR。
 */

#include "control.h"

#include <stdbool.h>
#include <stdint.h>

#include "Sampling.h"
#include "adc_input.h"
#include "fan.h"
#include "key.h"
#include "main.h"

/* 风扇控制和电源采样周期必须是 10 ms 基准节拍的整数倍。 */
#define CONTROL_SAMPLE_PERIOD_MS     200U
#define CONTROL_FAN_PERIOD_TICKS     (FAN_PROCESS_PERIOD_MS / KEY_SCAN_PERIOD_MS)
#define CONTROL_SAMPLE_PERIOD_TICKS  (CONTROL_SAMPLE_PERIOD_MS / KEY_SCAN_PERIOD_MS)
#define ENCODER_ACCEL_WINDOW_TICKS   (ENCODER_ACCEL_WINDOW_MS / KEY_SCAN_PERIOD_MS)
#define ENCODER_ACCEL_MEDIUM_LIMIT   1U
#define ENCODER_ACCEL_FAST_LIMIT     4U
#define ENCODER_ACCEL_VERY_FAST_LIMIT 8U

_Static_assert((FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ENCODER) ||
               (FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ADC),
               "fan duty source must be encoder or ADC");
_Static_assert((FAN_PROCESS_PERIOD_MS % KEY_SCAN_PERIOD_MS) == 0U,
               "fan period must align with key scan period");
_Static_assert((CONTROL_SAMPLE_PERIOD_MS % KEY_SCAN_PERIOD_MS) == 0U,
               "sample period must align with key scan period");
_Static_assert((ENCODER_ACCEL_WINDOW_MS >= KEY_SCAN_PERIOD_MS) &&
               ((ENCODER_ACCEL_WINDOW_MS % KEY_SCAN_PERIOD_MS) == 0U),
               "encoder acceleration window must align with key scan period");
_Static_assert((ENCODER_STEP_PERCENT_SLOW > 0U) &&
               (ENCODER_STEP_PERCENT_SLOW <= ENCODER_STEP_PERCENT_MEDIUM) &&
               (ENCODER_STEP_PERCENT_MEDIUM <= ENCODER_STEP_PERCENT_FAST) &&
               (ENCODER_STEP_PERCENT_FAST <= ENCODER_STEP_PERCENT_VERY_FAST),
               "encoder step percentages must be positive and ordered");

/* 显示层只读取这一份由控制层汇总的状态，避免跨模块拼接不一致快照。 */
static control_state_t control_state;

/* TIM14 ISR 写入，主循环在短临界区内复制并清空。 */
static volatile int16_t pending_encoder_steps;
static volatile bool pending_button_short_pressed;
static volatile uint32_t timer_tick_sequence;

/* 以下调度状态只由主循环访问。节拍序号允许 uint32_t 自然回绕。 */
static uint32_t processed_tick_sequence;
static uint32_t fan_tick_accumulator;
static uint32_t sample_tick_accumulator;
static bool refresh_pending;

#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ENCODER
/* 连续同向旋转累计刻度；停顿或反向时恢复慢速档。 */
static uint16_t encoder_burst_steps;
static uint32_t encoder_last_activity_sequence;
static int8_t encoder_last_direction;
#endif

#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ENCODER
static int32_t Control_ClampS32(int32_t value, int32_t minimum, int32_t maximum)
{
    if (value < minimum)
    {
        return minimum;
    }
    if (value > maximum)
    {
        return maximum;
    }
    return value;
}
#endif

/* PA4 为高电平有效；仅控制层写入该输出。 */
static void Control_ApplyLedState(void)
{
    HAL_GPIO_WritePin(LED_ENABLE_GPIO_Port,
                      LED_ENABLE_Pin,
                      control_state.led_enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 把风扇模块的千分比占空比四舍五入为界面使用的整数百分比。 */
static void Control_CopyFanState(void)
{
    const fan_state_t *fan = Fan_GetState();

    control_state.actual_rpm = fan->actual_rpm;
    control_state.target_duty_percent = fan->target_duty_percent;
    control_state.fan_pwm_percent = (uint8_t)((fan->duty_permille + 5U) / 10U);
    control_state.fan_running = fan->duty_permille > 0U;
    control_state.tach_valid = fan->tach_valid;
}

/* 连同诊断标志复制测量结果，使同一帧状态与工程量保持一致。 */
static void Control_CopyMeasurements(void)
{
    const sampling_data_t *samples = Sampling_GetData();

    control_state.input_voltage_mv = samples->input_voltage_mv;
    control_state.current_ma = samples->current_ma;
    control_state.power_mw = samples->power_mw;
    control_state.ina219_compatible = samples->ina219_compatible;
    control_state.ina226_online = samples->online;
    control_state.ina226_identity_valid = samples->identity_valid;
    control_state.ina226_configuration_valid = samples->configuration_valid;
    control_state.measurements_valid = samples->data_valid;
}

/*
 * 在 ISR 中合并本次输入。编码器量采用饱和累加，避免主循环暂时繁忙时发生
 * 有符号回绕；短按事件置位后一直保留到主循环取走。
 */
static void Control_QueueInputFromInterrupt(key_input_t input)
{
#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ENCODER
    if (input.encoder_steps != 0)
    {
        int32_t accumulated_steps =
            (int32_t)pending_encoder_steps + (int32_t)input.encoder_steps;
        accumulated_steps = Control_ClampS32(accumulated_steps, INT16_MIN, INT16_MAX);
        pending_encoder_steps = (int16_t)accumulated_steps;
    }
#endif

    if (input.button_short_pressed)
    {
        pending_button_short_pressed = true;
    }
}

/* 复制并清空待决输入；调用前中断若已关闭，退出时保持原状态。 */
static void Control_TakePendingInput(int16_t *steps, bool *button_short_pressed)
{
    uint32_t previous_primask = __get_PRIMASK();

    __disable_irq();
    *steps = pending_encoder_steps;
    *button_short_pressed = pending_button_short_pressed;
    pending_encoder_steps = 0;
    pending_button_short_pressed = false;

    if (previous_primask == 0U)
    {
        __enable_irq();
    }
}

#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ENCODER
/* 根据连续同向旋转的累计刻度选择本次每格占空比步长。 */
static uint8_t Control_GetEncoderStepPercent(int16_t steps)
{
    uint32_t now_sequence = timer_tick_sequence;
    uint32_t magnitude = (steps < 0) ? (uint32_t)(-(int32_t)steps)
                                     : (uint32_t)steps;
    int8_t direction = (steps < 0) ? -1 : 1;

    if ((encoder_burst_steps == 0U) ||
        (direction != encoder_last_direction) ||
        ((now_sequence - encoder_last_activity_sequence) > ENCODER_ACCEL_WINDOW_TICKS))
    {
        encoder_burst_steps = 0U;
    }

    if (magnitude > (uint32_t)(UINT16_MAX - encoder_burst_steps))
    {
        encoder_burst_steps = UINT16_MAX;
    }
    else
    {
        encoder_burst_steps = (uint16_t)(encoder_burst_steps + magnitude);
    }

    encoder_last_activity_sequence = now_sequence;
    encoder_last_direction = direction;

    if (encoder_burst_steps <= ENCODER_ACCEL_MEDIUM_LIMIT)
    {
        return ENCODER_STEP_PERCENT_SLOW;
    }
    if (encoder_burst_steps <= ENCODER_ACCEL_FAST_LIMIT)
    {
        return ENCODER_STEP_PERCENT_MEDIUM;
    }
    if (encoder_burst_steps <= ENCODER_ACCEL_VERY_FAST_LIMIT)
    {
        return ENCODER_STEP_PERCENT_FAST;
    }
    return ENCODER_STEP_PERCENT_VERY_FAST;
}

/* 应用编码器加速步长，并把目标限制在 0..97%。 */
static bool Control_AdjustFanTarget(int16_t steps)
{
    int32_t target;
    uint8_t step_percent;

    if (steps == 0)
    {
        return false;
    }

    step_percent = Control_GetEncoderStepPercent(steps);
    target = (int32_t)control_state.target_duty_percent +
             (int32_t)steps * (int32_t)step_percent;
    target = Control_ClampS32(target,
                              FAN_MIN_TARGET_DUTY_PERCENT,
                              FAN_MAX_TARGET_DUTY_PERCENT);
    if ((uint8_t)target == control_state.target_duty_percent)
    {
        return false;
    }

    Fan_SetTargetDutyPercent((uint8_t)target);
    Control_CopyFanState();
    return true;
}
#endif

void Control_Init(void)
{
    control_state = (control_state_t){
        .led_enabled = false,
    };

    pending_encoder_steps = 0;
    pending_button_short_pressed = false;
    timer_tick_sequence = 0U;
    processed_tick_sequence = 0U;
    fan_tick_accumulator = 0U;
    sample_tick_accumulator = 0U;
#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ENCODER
    encoder_burst_steps = 0U;
    encoder_last_activity_sequence = 0U;
    encoder_last_direction = 0;
#endif

    Control_ApplyLedState();
    Key_Init();
    Fan_Init();
#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ADC
    AdcInput_Init();
    Fan_SetTargetDutyPercent(AdcInput_ReadDutyPercent());
#endif

    /* 初始化失败由采样状态记录，主循环会按重试间隔自动恢复。 */
    (void)Sampling_Init();
    Control_CopyFanState();
    Control_CopyMeasurements();
    refresh_pending = true;
}

void Control_NotifyTimerInterrupt(void)
{
    Control_QueueInputFromInterrupt(Key_Scan());
    timer_tick_sequence++;
}

bool Control_Process(void)
{
    int16_t encoder_steps;
    bool button_short_pressed;
    uint32_t pending_sequence;
    uint32_t elapsed_ticks;
    bool changed = refresh_pending;

    refresh_pending = false;
    Control_TakePendingInput(&encoder_steps, &button_short_pressed);

    if (button_short_pressed)
    {
        control_state.led_enabled = !control_state.led_enabled;
        Control_ApplyLedState();
        changed = true;
    }

#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ENCODER
    changed |= Control_AdjustFanTarget(encoder_steps);
#else
    (void)encoder_steps;
#endif

    /*
     * 使用单调递增序号而不是单个布尔标志，主循环被 SPI/I²C 阻塞期间产生的
     * 多个节拍仍可计入累计时间；无符号减法可自然跨越计数器回绕。
     */
    pending_sequence = timer_tick_sequence;
    elapsed_ticks = pending_sequence - processed_tick_sequence;
    if (elapsed_ticks != 0U)
    {
        processed_tick_sequence = pending_sequence;
        fan_tick_accumulator += elapsed_ticks;
        sample_tick_accumulator += elapsed_ticks;
    }

    /* 到期任务每轮最多执行一次，长时间阻塞后合并遗漏周期而不突发补跑。 */
    if (fan_tick_accumulator >= CONTROL_FAN_PERIOD_TICKS)
    {
        fan_tick_accumulator %= CONTROL_FAN_PERIOD_TICKS;
#if FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ADC
        Fan_SetTargetDutyPercent(AdcInput_ReadDutyPercent());
#endif
        Fan_Process();
        Control_CopyFanState();
        changed = true;
    }

    if (sample_tick_accumulator >= CONTROL_SAMPLE_PERIOD_TICKS)
    {
        sample_tick_accumulator %= CONTROL_SAMPLE_PERIOD_TICKS;
        Sampling_Update();
        Control_CopyMeasurements();
        changed = true;
    }

    return changed;
}

const control_state_t *Control_GetState(void)
{
    return &control_state;
}
