/*
 * show.h
 *
 * 160×80 风扇控制仪表界面。模块从 Control_GetState() 读取只读快照，负责
 * 静态布局、局部刷新以及 FAN/LED 动画；底层绘制采用阻塞式 SPI。
 */

#ifndef SHOW_H
#define SHOW_H

/**
 * @brief 初始化 TFT，并绘制不随运行状态变化的仪表框架。
 *
 * 函数包含硬件复位、延时和阻塞式 SPI 传输，只能在主循环上下文调用。系统
 * 启动时应在 Control_Init() 开启风扇测速中断之前调用。
 */
void Show_Init(void);

/**
 * @brief 根据当前控制状态刷新发生变化的区域并推进动画一帧。
 *
 * 函数依赖 Show_Init() 建立的布局和缓存，只能在主循环上下文调用。
 */
void Show_Update(void);

#endif /* SHOW_H */
