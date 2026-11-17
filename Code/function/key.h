/*
 * key.h
 *
 * 旋转编码器与按键输入接口。TIM3 以 X4 模式累计 A/B 相计数，PC15 低电平
 * 表示按键按下；模块由 10 ms 应用节拍周期采样，输出旋转刻度和短按事件。
 */

#ifndef KEY_H
#define KEY_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_SCAN_PERIOD_MS 10U

/*
 * 编码器方向由实际 A/B 相接线决定。当前硬件使用反向配置，使顺时针旋转增加
 * 目标值；也可通过编译参数把 KEY_ENCODER_DIRECTION 覆盖为正向。
 */
#define KEY_ENCODER_DIRECTION_FORWARD  1L
#define KEY_ENCODER_DIRECTION_REVERSED (-1L)
#ifndef KEY_ENCODER_DIRECTION
#define KEY_ENCODER_DIRECTION KEY_ENCODER_DIRECTION_REVERSED
#endif

/** 单次扫描产生的增量事件。 */
typedef struct
{
    int8_t encoder_steps;       /**< 本周期累计的机械刻度数，正负号表示方向。 */
    bool button_short_pressed;  /**< 消抖后按下并释放时产生一次。 */
} key_input_t;

/**
 * @brief 启动 TIM3 编码器计数并初始化按键消抖状态。
 *
 * 调用前必须已经由 CubeMX 初始化 TIM3、编码器 GPIO 和 PC15 按键输入。
 */
void Key_Init(void);

/**
 * @brief 读取编码器增量并对低有效按键执行一次 10 ms 采样。
 *
 * 函数只访问定时器和 GPIO，可由 TIM14 ISR 调用。连续四个 X4 计数构成一个
 * 机械刻度；按键执行 30 ms 消抖，并在一次完整按下释放后报告短按事件。
 *
 * @return 本次扫描产生的旋转刻度和短按事件。
 */
key_input_t Key_Scan(void);

#endif /* KEY_H */
