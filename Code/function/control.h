/*
 * control.h
 *
 * 应用控制层接口，定义调速输入、风扇、LED、电源采样与显示之间共享的状态
 * 快照。TIM14 中断只投递编码器、按键和节拍，阻塞外设操作均在主循环完成。
 */

#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/* 编译期选择风扇目标来自旋转编码器或 PA5/ADC1_IN5 电位器。 */
#define FAN_DUTY_SOURCE_ENCODER 0U
#define FAN_DUTY_SOURCE_ADC     1U
#ifndef FAN_DUTY_SOURCE
#define FAN_DUTY_SOURCE FAN_DUTY_SOURCE_ENCODER
#endif

/*
 * 编码器模式的加速参数。连续旋转间隔不超过窗口时累计刻度，并按累计量选择
 * 1/5/10/20% 单格步长；停顿或反向后恢复到最小步长。
 */
#ifndef ENCODER_ACCEL_WINDOW_MS
#define ENCODER_ACCEL_WINDOW_MS 150U
#endif
#ifndef ENCODER_STEP_PERCENT_SLOW
#define ENCODER_STEP_PERCENT_SLOW 1U
#endif
#ifndef ENCODER_STEP_PERCENT_MEDIUM
#define ENCODER_STEP_PERCENT_MEDIUM 5U
#endif
#ifndef ENCODER_STEP_PERCENT_FAST
#define ENCODER_STEP_PERCENT_FAST 10U
#endif
#ifndef ENCODER_STEP_PERCENT_VERY_FAST
#define ENCODER_STEP_PERCENT_VERY_FAST 20U
#endif

/** 显示层使用的完整应用状态快照。 */
typedef struct
{
    uint32_t input_voltage_mv;          /**< 输入电压，单位 mV。 */
    int32_t current_ma;                 /**< 输入电流，单位 mA。 */
    int32_t power_mw;                   /**< 输入功率，单位 mW。 */
    uint16_t actual_rpm;                /**< 始终运行的 TIM17 测得的实际转速。 */
    uint8_t target_duty_percent;        /**< 目标 PWM 占空比，范围 0..97%。 */
    uint8_t fan_pwm_percent;            /**< 当前实际 PWM 占空比，范围 0..97%。 */
    bool fan_running;                   /**< 当前实际 PWM 是否大于 0%。 */
    bool led_enabled;                   /**< LED 输出使能状态。 */
    bool tach_valid;                    /**< 最近 500 ms 内是否收到有效测速周期。 */
    bool ina219_compatible;             /**< 电源监控器采用 INA219-compatible 格式。 */
    bool ina226_online;                 /**< 电源监控器最近一次 I²C 通信正常。 */
    bool ina226_identity_valid;         /**< 标准 INA226 身份寄存器有效。 */
    bool ina226_configuration_valid;    /**< 配置和校准寄存器回读正确。 */
    bool measurements_valid;            /**< 电压、电流和功率来自完整有效采样。 */
} control_state_t;

/**
 * @brief 初始化输入、风扇、LED、电源监控和所选调速来源。
 *
 * 调用前应完成相关 GPIO、ADC、I²C 和定时器初始化。Fan_Init() 会开启 TIM17
 * 中断，因此应先完成 TFT 初始化，避免显示延时被中断影响。
 */
void Control_Init(void);

/**
 * @brief 扫描一次编码器和按键，并记录一个 10 ms 调度节拍。
 *
 * 仅允许由 TIM14 中断调用。函数只访问寄存器和累积紧凑事件，不执行 ADC、
 * I²C、SPI 或其他阻塞操作。
 */
void Control_NotifyTimerInterrupt(void);

/**
 * @brief 在主循环中处理输入以及到期的风扇、ADC 和电源采样任务。
 *
 * @return 任一显示相关状态发生变化时返回 true，否则返回 false。
 */
bool Control_Process(void);

/**
 * @brief 获取供显示层读取的当前应用状态。
 *
 * @return 指向模块内部只读状态的指针；内容可能在下次 Control_Process() 后更新。
 */
const control_state_t *Control_GetState(void);

#endif /* CONTROL_H */
