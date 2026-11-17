/*
 * fan.c
 *
 * 风扇底层驱动与开环控制。TIM16 在 PB8 输出 25 kHz PWM，输出类型可独立
 * 配置为推挽或开漏；目标占空比直接决定 PWM 请求。TIM17 在 PB9 持续测速，
 * 实际 RPM 仅用于监测和显示。主循环每 100 ms 更新测速与输出，中断路径只
 * 维护测速时间戳和状态标志。
 */

#include "fan.h"

#include <stdbool.h>
#include <stdint.h>

#include "main.h"

/* 测速定时器以 1 MHz 计数；风扇每转输出两个上升沿。 */
#define FAN_TACH_TIMER_HZ              1000000UL
#define FAN_TACH_PULSES_PER_REVOLUTION 2UL
#define FAN_TACH_TIMEOUT_TICKS         500000UL
/* 小于 1 ms 的周期对应超过 30000 RPM，按毛刺处理。 */
#define FAN_TACH_MIN_PERIOD_TICKS      1000UL

/* 1000‰ 是换算比例尺；输出上限由公开的目标百分比上限统一决定。 */
#define FAN_DUTY_SCALE_PERMILLE 1000U
#define FAN_MIN_DUTY_PERMILLE   0U
#define FAN_MAX_DUTY_PERMILLE   (FAN_MAX_TARGET_DUTY_PERCENT * 10U)
#define FAN_RAMP_NUMERATOR_PER_STEP \
    (FAN_DUTY_SCALE_PERMILLE * FAN_PROCESS_PERIOD_MS)
#define FAN_RAMP_STEP_PERMILLE        \
    (FAN_RAMP_NUMERATOR_PER_STEP / FAN_DUTY_RAMP_UP_TIME_MS)
#define FAN_RAMP_STEP_REMAINDER       \
    (FAN_RAMP_NUMERATOR_PER_STEP % FAN_DUTY_RAMP_UP_TIME_MS)

_Static_assert((FAN_PWM_DRIVE_MODE == FAN_PWM_DRIVE_PUSH_PULL) ||
               (FAN_PWM_DRIVE_MODE == FAN_PWM_DRIVE_OPEN_DRAIN),
               "fan PWM drive mode must be push-pull or open-drain");
_Static_assert(FAN_MAX_TARGET_DUTY_PERCENT < 100U,
               "fan maximum duty must stay below 100 percent");
_Static_assert(FAN_DUTY_RAMP_UP_TIME_MS >= FAN_PROCESS_PERIOD_MS,
               "fan ramp time must cover at least one control period");
_Static_assert(FAN_DUTY_RAMP_UP_TIME_MS <= FAN_RAMP_NUMERATOR_PER_STEP,
               "fan ramp time must produce a nonzero duty step");

/* 供控制层读取的模块状态只在主循环更新。 */
static fan_state_t fan_state;
#if FAN_RAMP_STEP_REMAINDER != 0U
/* 仅在配置时间不能整除时保留余数；默认 2 s 配置不会占用该状态。 */
static uint32_t duty_ramp_remainder;
#endif

/*
 * TIM17 的 16 位计数器由软件高字扩展为 32 位时间戳。测速始终运行且不参与
 * PWM 输出计算；以下变量由 ISR 写入，主循环读取多字段快照时必须进入短临界区。
 */
static volatile uint32_t tach_overflow_count;
static volatile uint32_t tach_last_capture_timestamp;
static volatile uint32_t tach_period_ticks;
static volatile bool tach_has_first_capture;
static volatile bool tach_period_valid;

/* 按 TIM16 实际周期把允许范围内的千分比换算为比较值并同步公开状态。 */
static void Fan_ApplyDuty(uint16_t duty_permille)
{
    uint32_t period_counts;
    uint32_t compare;

    if (duty_permille > FAN_MAX_DUTY_PERMILLE)
    {
        duty_permille = FAN_MAX_DUTY_PERMILLE;
    }

    period_counts = TIM16->ARR + 1UL;
    /* 始终按 1000‰ 比例尺换算，970‰ 对应真实 97% 而不是连续有效电平。 */
    compare = ((uint32_t)duty_permille * period_counts) /
              FAN_DUTY_SCALE_PERMILLE;
    TIM16->CCR1 = compare;
    fan_state.duty_permille = duty_permille;
}

/*
 * 对最终占空比请求施加统一上升斜率。下降立即生效；上升步长在编译期由
 * 控制周期和目标时长计算，不能整除时用余数累加补偿长期速率。默认每
 * 100 ms 增加 50‰，受 970‰ 上限约束并在第 20 个周期达到 97%。
 */
static void Fan_ApplyLimitedDuty(uint16_t requested_duty)
{
    uint16_t limited_duty;

    if (requested_duty > FAN_MAX_DUTY_PERMILLE)
    {
        requested_duty = FAN_MAX_DUTY_PERMILLE;
    }

    if (requested_duty <= fan_state.duty_permille)
    {
#if FAN_RAMP_STEP_REMAINDER != 0U
        duty_ramp_remainder = 0U;
#endif
        Fan_ApplyDuty(requested_duty);
        return;
    }

    limited_duty = (uint16_t)(fan_state.duty_permille + FAN_RAMP_STEP_PERMILLE);
#if FAN_RAMP_STEP_REMAINDER != 0U
    duty_ramp_remainder += FAN_RAMP_STEP_REMAINDER;
    if (duty_ramp_remainder >= FAN_DUTY_RAMP_UP_TIME_MS)
    {
        duty_ramp_remainder -= FAN_DUTY_RAMP_UP_TIME_MS;
        limited_duty++;
    }
#endif

    if ((limited_duty >= requested_duty) ||
        (limited_duty > FAN_MAX_DUTY_PERMILLE))
    {
        limited_duty = requested_duty;
#if FAN_RAMP_STEP_REMAINDER != 0U
        duty_ramp_remainder = 0U;
#endif
    }
    Fan_ApplyDuty(limited_duty);
}

/* 原子读取软件高字和当前计数器，生成用于超时判断的 32 位时间戳。 */
static uint32_t Fan_GetTachTimestamp(void)
{
    uint32_t previous_primask = __get_PRIMASK();
    uint32_t high_word;
    uint16_t counter;

    __disable_irq();
    high_word = tach_overflow_count;
    counter = (uint16_t)TIM17->CNT;

    /* 若 CNT 读取前已经溢出但 ISR 尚未处理，则把本次溢出计入快照。 */
    if (((TIM17->SR & TIM_SR_UIF) != 0U) && (counter < 0x8000U))
    {
        high_word++;
    }

    if (previous_primask == 0U)
    {
        __enable_irq();
    }

    return (high_word << 16U) | counter;
}

/* 在同一临界区取得周期和末次捕获时间，避免组合出跨中断的混合快照。 */
static bool Fan_ReadTachPeriod(uint32_t *period_ticks, uint32_t *last_capture)
{
    uint32_t previous_primask = __get_PRIMASK();
    bool valid;

    __disable_irq();
    valid = tach_period_valid;
    *period_ticks = tach_period_ticks;
    *last_capture = tach_last_capture_timestamp;
    if (previous_primask == 0U)
    {
        __enable_irq();
    }

    return valid;
}

/* 校验捕获周期与超时状态，计算 RPM，并对有效读数执行一阶低通滤波。 */
static void Fan_UpdateMeasuredSpeed(void)
{
    uint32_t period_ticks;
    uint32_t last_capture;
    uint32_t now_timestamp;
    uint32_t rpm;
    bool valid;

    valid = Fan_ReadTachPeriod(&period_ticks, &last_capture);
    now_timestamp = Fan_GetTachTimestamp();

    if (!valid ||
        (period_ticks < FAN_TACH_MIN_PERIOD_TICKS) ||
        ((now_timestamp - last_capture) > FAN_TACH_TIMEOUT_TICKS))
    {
        fan_state.actual_rpm = 0U;
        fan_state.tach_valid = false;
        return;
    }

    rpm = (60UL * FAN_TACH_TIMER_HZ) /
          (FAN_TACH_PULSES_PER_REVOLUTION * period_ticks);
    if (rpm > UINT16_MAX)
    {
        rpm = UINT16_MAX;
    }

    if (!fan_state.tach_valid)
    {
        fan_state.actual_rpm = (uint16_t)rpm;
    }
    else
    {
        /* 1/4 IIR：保留 3/4 旧值，抑制单周期测速抖动。 */
        fan_state.actual_rpm =
            (uint16_t)(((uint32_t)fan_state.actual_rpm * 3UL + rpm + 2UL) / 4UL);
    }
    fan_state.tach_valid = true;
}

void Fan_Init(void)
{
    fan_state = (fan_state_t){
        .actual_rpm = 0U,
        .duty_permille = 0U,
        .target_duty_percent = FAN_DEFAULT_TARGET_DUTY_PERCENT,
        .tach_valid = false,
    };

    tach_overflow_count = 0U;
    tach_last_capture_timestamp = 0U;
    tach_period_ticks = 0U;
    tach_has_first_capture = false;
    tach_period_valid = false;

    /* 输出电气类型独立于反馈策略；固定实例寄存器写法可减小 Debug 代码量。 */
#if FAN_PWM_DRIVE_MODE == FAN_PWM_DRIVE_OPEN_DRAIN
    FAN_PWM_GPIO_Port->OTYPER |= FAN_PWM_Pin;
#else
    FAN_PWM_GPIO_Port->OTYPER &= ~(uint32_t)FAN_PWM_Pin;
#endif

    TIM16->CNT = 0U;
    TIM16->CCR1 = 0U;
    TIM16->SR = ~TIM_SR_UIF;
    TIM16->BDTR |= TIM_BDTR_MOE;
    TIM16->CCER |= TIM_CCER_CC1E;
    TIM16->CR1 |= TIM_CR1_CEN;

    /* TIM17 独立于 PWM 控制，始终持续捕获 PB9 测速信号。 */
    TIM17->CNT = 0U;
    TIM17->SR = ~(TIM_SR_UIF | TIM_SR_CC1IF | TIM_SR_CC1OF);
    TIM17->CCER |= TIM_CCER_CC1E;
    TIM17->DIER |= TIM_DIER_UIE | TIM_DIER_CC1IE;
    TIM17->CR1 |= TIM_CR1_CEN;
}

void Fan_SetTargetDutyPercent(uint8_t duty_percent)
{
    if (duty_percent > FAN_MAX_TARGET_DUTY_PERCENT)
    {
        duty_percent = FAN_MAX_TARGET_DUTY_PERCENT;
    }

    fan_state.target_duty_percent = duty_percent;
}

void Fan_Process(void)
{
    uint16_t requested_duty;

    Fan_UpdateMeasuredSpeed();

    requested_duty = (uint16_t)fan_state.target_duty_percent * 10U;
    Fan_ApplyLimitedDuty(requested_duty);
}

void Fan_HandleTimerInterrupt(void)
{
    bool update_pending =
        ((TIM17->SR & TIM_SR_UIF) != 0U) &&
        ((TIM17->DIER & TIM_DIER_UIE) != 0U);
    bool capture_pending =
        ((TIM17->SR & TIM_SR_CC1IF) != 0U) &&
        ((TIM17->DIER & TIM_DIER_CC1IE) != 0U);

    if (capture_pending)
    {
        uint16_t capture = (uint16_t)TIM17->CCR1;
        uint32_t high_word = tach_overflow_count;
        uint32_t timestamp;

        /* 捕获与溢出同时挂起时，根据捕获值所在半周判断事件先后。 */
        if (update_pending && (capture < 0x8000U))
        {
            high_word++;
        }
        timestamp = (high_word << 16U) | capture;

        if (tach_has_first_capture)
        {
            uint32_t period = timestamp - tach_last_capture_timestamp;
            /* 毛刺周期不更新末次有效时间，持续噪声不能阻止 500 ms 超时。 */
            if (period >= FAN_TACH_MIN_PERIOD_TICKS)
            {
                tach_period_ticks = period;
                tach_period_valid = true;
                tach_last_capture_timestamp = timestamp;
            }
        }
        else
        {
            tach_has_first_capture = true;
            tach_last_capture_timestamp = timestamp;
        }

        TIM17->SR = ~TIM_SR_CC1IF;
    }

    if (update_pending)
    {
        tach_overflow_count++;
        TIM17->SR = ~TIM_SR_UIF;
    }

    if ((TIM17->SR & TIM_SR_CC1OF) != 0U)
    {
        TIM17->SR = ~TIM_SR_CC1OF;
    }
}

const fan_state_t *Fan_GetState(void)
{
    return &fan_state;
}
