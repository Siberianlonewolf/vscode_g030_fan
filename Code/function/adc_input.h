/*
 * adc_input.h
 *
 * PA5/ADC1_IN5 电位器输入接口。模块在主循环中执行单次阻塞转换、低通滤波和
 * 低端死区处理，并把 12 位 ADC 数值线性映射为风扇目标占空比。
 */

#ifndef ADC_INPUT_H
#define ADC_INPUT_H

#include <stdint.h>

/* 低于该原始值时强制输出 0%，吸收电位器和 ADC 的零点误差。 */
#ifndef ADC_INPUT_DEADBAND_RAW
#define ADC_INPUT_DEADBAND_RAW 32U
#endif

/* EMA 权重为 1 / 2^N；默认 N=2，即每次保留 3/4 旧值。 */
#ifndef ADC_INPUT_FILTER_SHIFT
#define ADC_INPUT_FILTER_SHIFT 2U
#endif

#ifndef ADC_INPUT_POLL_TIMEOUT_MS
#define ADC_INPUT_POLL_TIMEOUT_MS 2U
#endif

/** 校准 ADC1 并复位滤波状态；必须在 MX_ADC1_Init() 之后调用。 */
void AdcInput_Init(void);

/**
 * @brief 采集并转换电位器位置。
 *
 * 转换失败时保留最近一次有效目标；初始化或校准失败时返回 0%。
 *
 * @return 0..FAN_MAX_TARGET_DUTY_PERCENT 的目标占空比。
 */
uint8_t AdcInput_ReadDutyPercent(void);

#endif /* ADC_INPUT_H */
