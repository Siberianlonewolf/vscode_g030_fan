/*
 * Sampling.h
 *
 * 电源监控采样接口。当前硬件默认使用标准 INA226，同时保留
 * INA219-compatible 备用格式，统一向上层提供以 mV、mA、mW 表示的整数
 * 工程量。采样状态由模块持有，I²C 访问和换算只能在主循环中执行。
 */

#ifndef SAMPLING_H
#define SAMPLING_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t input_voltage_mv;    /**< 校准后的母线电压，单位 mV。 */
    int32_t current_ma;           /**< 分流电压换算所得电流，单位 mA，允许为负。 */
    int32_t power_mw;             /**< 电压与电流计算所得功率，单位 mW，允许为负。 */
    uint16_t raw_bus_voltage;     /**< 最近一次读取的母线电压寄存器原始值。 */
    uint16_t manufacturer_id;     /**< 标准 INA226 的制造商标识；兼容模式下为 0。 */
    uint16_t device_id;           /**< 标准 INA226 的器件标识；兼容模式下为 0。 */
    bool ina219_compatible;       /**< 当前按 INA219-compatible 寄存器格式解码。 */
    bool online;                  /**< 最近一次必要的 I²C 通信成功。 */
    bool identity_valid;          /**< 标准 INA226 身份寄存器与预期值一致。 */
    bool configuration_valid;     /**< 配置及校准寄存器回读值与写入值一致。 */
    bool data_valid;              /**< 工程量来自一次完整且成功的寄存器采样。 */
} sampling_data_t;

/**
 * @brief 配置地址为 0x40 的电源监控器，并清空历史采样。
 *
 * 配置按板载 R010（10 mΩ）分流电阻计算。配置或身份回读不一致会记录为
 * 诊断状态，但只有 I²C 通信失败才使初始化返回失败。
 *
 * @return 必要的配置写入和回读成功时返回 true，否则返回 false。
 */
bool Sampling_Init(void);

/**
 * @brief 执行一次采样，或在设备离线时按间隔尝试重新配置。
 *
 * 本函数包含阻塞式 I²C 访问，只能由主循环调用；当前调度周期为 200 ms。
 */
void Sampling_Update(void);

/**
 * @brief 获取最近一次采样及诊断状态。
 *
 * @return 指向模块内部只读数据的指针；内容可能在下一次初始化或采样后更新。
 */
const sampling_data_t *Sampling_GetData(void);

#endif /* SAMPLING_H */
