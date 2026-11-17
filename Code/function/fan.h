/*
 * fan.h
 *
 * 风扇驱动与开环控制接口。TIM16 CH1 在 PB8 输出 25 kHz PWM，目标占空比
 * 可在 0..97% 范围内调节；0% 表示停止，大于 0% 表示运行。TIM17 CH1 始终
 * 采集 PB9 测速信号，实际转速只用于监测和显示，不参与 PWM 输出计算。
 */

#ifndef FAN_H
#define FAN_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 推挽输出用于外部 MOSFET 或门极驱动器的逻辑输入；开漏输出用于带有正确
 * 外部上拉的四线风扇 PWM 控制脚。PB8 不能直接为二线或三线风扇供电。
 */
#define FAN_PWM_DRIVE_PUSH_PULL  0U
#define FAN_PWM_DRIVE_OPEN_DRAIN 1U
#ifndef FAN_PWM_DRIVE_MODE
#define FAN_PWM_DRIVE_MODE FAN_PWM_DRIVE_PUSH_PULL
#endif

/* 风扇任务周期是测速刷新和占空比上升斜率计算的共同时间基准。 */
#define FAN_PROCESS_PERIOD_MS 100U
#ifndef FAN_DUTY_RAMP_UP_TIME_MS
#define FAN_DUTY_RAMP_UP_TIME_MS 2000U
#endif

/* 目标直接表示 PWM 百分比，最高限制为 97%，不允许连续高电平。 */
#define FAN_MIN_TARGET_DUTY_PERCENT     0U
#define FAN_MAX_TARGET_DUTY_PERCENT     97U
#define FAN_TARGET_DUTY_STEP_PERCENT    1U
#define FAN_DEFAULT_TARGET_DUTY_PERCENT 0U

/** 风扇开环输出与独立测速状态。 */
typedef struct
{
    uint16_t actual_rpm;           /**< TIM17 测得并经 IIR 滤波的实际转速。 */
    uint16_t duty_permille;        /**< 当前实际 PWM 占空比，范围 0..970。 */
    uint8_t target_duty_percent;   /**< 目标 PWM 占空比，范围 0..97%。 */
    bool tach_valid;               /**< 最近 500 ms 内是否收到有效测速周期。 */
} fan_state_t;

/**
 * @brief 初始化 TIM16 PWM 和始终运行的 TIM17 测速。
 *
 * PB8 按 FAN_PWM_DRIVE_MODE 配置为推挽或开漏。TIM17 启用捕获及更新中断；
 * PB9 没有输入时实际转速保持为 0。
 */
void Fan_Init(void);

/**
 * @brief 设置开环目标占空比。
 *
 * 输入统一限制在 0..97%；0% 会立即停止，大于 0% 的上升过程经过斜率限制。
 *
 * @param duty_percent 目标占空比，单位百分比。
 */
void Fan_SetTargetDutyPercent(uint8_t duty_percent);

/**
 * @brief 更新测速结果并执行一次开环占空比控制。
 *
 * 只能由主循环每 FAN_PROCESS_PERIOD_MS 调用一次；调用周期是上升斜率计算的
 * 时间基准。实际 RPM 仅更新监测状态，不参与占空比计算。
 */
void Fan_Process(void);

/**
 * @brief 处理 TIM17 捕获、计数器溢出及捕获溢出标志。
 *
 * 仅由 TIM17_IRQHandler() 调用。函数只记录时间戳并清除标志，不执行显示、
 * 控制计算或其他阻塞操作。
 */
void Fan_HandleTimerInterrupt(void);

/**
 * @brief 获取风扇当前状态。
 *
 * @return 指向模块内部只读状态的指针；内容会在后续风扇接口调用后更新。
 */
const fan_state_t *Fan_GetState(void);

#endif /* FAN_H */
