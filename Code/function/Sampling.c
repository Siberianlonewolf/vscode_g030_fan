/*
 * Sampling.c
 *
 * 输入电源监控器驱动。当前板载器件使用标准 INA226 寄存器格式，也可在
 * 编译期切换到 INA219-compatible 备用路径。模块通过 I²C2 读取母线电压和
 * 分流电压，并以 mV、mA、mW 整数工程量保存结果，避免在 32 KiB Flash 的
 * MCU 中引入浮点运行库。
 *
 * 所有 I²C 事务均由主循环发起；配置不一致只记录诊断，通信失败才会使设备
 * 离线并进入定时重试。
 */

#include "Sampling.h"

#include <stdbool.h>
#include <stdint.h>

#include "i2c.h"
#include "main.h"

/*
 * 当前硬件默认使用标准 INA226；需要兼容旧器件时可由编译参数覆盖为 1。
 * 模式选择集中在此处，避免上层根据字段名称推断器件类型。
 */
#ifndef POWER_MONITOR_INA219_COMPATIBLE
#define POWER_MONITOR_INA219_COMPATIBLE 0U
#endif

/* HAL 接口要求传入左移一位后的 8 位地址；器件的 7 位地址为 0x40。 */
#define POWER_MONITOR_I2C_ADDRESS          (0x40U << 1U)
#define POWER_MONITOR_REG_CONFIG           0x00U
#define POWER_MONITOR_REG_SHUNT_VOLTAGE    0x01U
#define POWER_MONITOR_REG_BUS_VOLTAGE      0x02U
#define POWER_MONITOR_REG_CALIBRATION      0x05U
#define INA226_REG_MANUFACTURER_ID         0xFEU
#define INA226_REG_DEVICE_ID               0xFFU

#if POWER_MONITOR_INA219_COMPATIBLE
/* INA219-compatible：32 V、±320 mV、母线/分流 12 位连续转换。 */
#define POWER_MONITOR_CONFIG_CONTINUOUS    0x399FU
/* Rshunt=10 mΩ、Current_LSB=250 µA：0.04096/(0.00025×0.01)=16384。 */
#define POWER_MONITOR_CALIBRATION_VALUE    0x4000U
#else
/* INA226：16 次平均、母线/分流转换时间 1.1 ms、连续转换。 */
#define POWER_MONITOR_CONFIG_CONTINUOUS    0x4527U
/* Rshunt=10 mΩ、Current_LSB=250 µA：0.00512/(0.00025×0.01)=2048。 */
#define POWER_MONITOR_CALIBRATION_VALUE    0x0800U
#endif

#define INA226_MANUFACTURER_ID              0x5449U
#define INA226_DEVICE_ID_MASK               0xFFF0U
#define INA226_DEVICE_ID_VALUE              0x2260U

/* I²C 单次事务超时、离线重试间隔和配置回读周期。 */
#define POWER_MONITOR_I2C_TIMEOUT_MS        20U
#define POWER_MONITOR_RETRY_INTERVAL_MS     1000U
#define POWER_MONITOR_VERIFY_INTERVAL       5U

/*
 * 母线电压的板级校准全程使用整数：
 *   corrected_mV = raw_mV × GAIN / 1000 + OFFSET_mV
 *
 * 标准 INA226 初始使用 1000‰ 原始比例。比例误差应按以下公式重新计算：
 *   GAIN = reference_mV × 1000 / uncalibrated_display_mV
 * 必须先确认寄存器格式解码正确，再根据基准表读数调整增益和偏移。
 */
#ifndef POWER_MONITOR_BUS_GAIN_PERMILLE
#define POWER_MONITOR_BUS_GAIN_PERMILLE     1000UL
#endif
#define POWER_MONITOR_BUS_OFFSET_MV         0L

_Static_assert(POWER_MONITOR_INA219_COMPATIBLE <= 1U,
               "monitor compatibility selector must be 0 or 1");
_Static_assert(POWER_MONITOR_BUS_GAIN_PERMILLE <= 6000UL,
               "bus gain must keep integer conversion in range");

/*
 * sampling_data 由本模块独占写入。configured 表示必要寄存器事务已成功，
 * verify_samples_remaining 用于降低周期性配置回读带来的 I²C 开销。
 */
static sampling_data_t sampling_data;
static bool power_monitor_configured;
static uint32_t last_config_attempt_ms;
static uint8_t verify_samples_remaining;

/** 按器件要求以高字节在前的顺序写入一个 16 位寄存器。 */
static bool PowerMonitor_WriteRegister(uint8_t register_address, uint16_t value)
{
    uint8_t bytes[2] = {
        (uint8_t)(value >> 8U),
        (uint8_t)value,
    };

    return HAL_I2C_Mem_Write(&hi2c2,
                             POWER_MONITOR_I2C_ADDRESS,
                             register_address,
                             I2C_MEMADD_SIZE_8BIT,
                             bytes,
                             sizeof(bytes),
                             POWER_MONITOR_I2C_TIMEOUT_MS) == HAL_OK;
}

/** 读取一个高字节在前的 16 位寄存器；空输出指针直接判为失败。 */
static bool PowerMonitor_ReadRegister(uint8_t register_address, uint16_t *value)
{
    uint8_t bytes[2];

    if (value == NULL)
    {
        return false;
    }

    if (HAL_I2C_Mem_Read(&hi2c2,
                         POWER_MONITOR_I2C_ADDRESS,
                         register_address,
                         I2C_MEMADD_SIZE_8BIT,
                         bytes,
                         sizeof(bytes),
                         POWER_MONITOR_I2C_TIMEOUT_MS) != HAL_OK)
    {
        return false;
    }

    *value = ((uint16_t)bytes[0] << 8U) | bytes[1];
    return true;
}

/* 通信失败后立即撤销数据有效性，并从当前 HAL 节拍开始计算重试间隔。 */
static void PowerMonitor_MarkOffline(void)
{
    power_monitor_configured = false;
    last_config_attempt_ms = HAL_GetTick();
    sampling_data.online = false;
    sampling_data.configuration_valid = false;
    sampling_data.data_valid = false;
}

/*
 * 写入工作模式和校准值并回读。标准 INA226 路径额外读取身份寄存器；兼容
 * 路径没有这些寄存器，因此身份字段保持为 0，不能据此判定设备离线。
 */
static bool PowerMonitor_Configure(void)
{
    uint16_t config;
    uint16_t calibration;
#if !POWER_MONITOR_INA219_COMPATIBLE
    uint16_t manufacturer_id;
    uint16_t device_id;
#endif

    last_config_attempt_ms = HAL_GetTick();
    sampling_data.configuration_valid = false;
    sampling_data.identity_valid = false;
    sampling_data.data_valid = false;
    sampling_data.manufacturer_id = 0U;
    sampling_data.device_id = 0U;

    if (!PowerMonitor_WriteRegister(POWER_MONITOR_REG_CONFIG, POWER_MONITOR_CONFIG_CONTINUOUS) ||
        !PowerMonitor_WriteRegister(POWER_MONITOR_REG_CALIBRATION, POWER_MONITOR_CALIBRATION_VALUE) ||
        !PowerMonitor_ReadRegister(POWER_MONITOR_REG_CONFIG, &config) ||
        !PowerMonitor_ReadRegister(POWER_MONITOR_REG_CALIBRATION, &calibration))
    {
        sampling_data.online = false;
        return false;
    }

    sampling_data.online = true;
    sampling_data.configuration_valid =
        (config == POWER_MONITOR_CONFIG_CONTINUOUS) &&
        (calibration == POWER_MONITOR_CALIBRATION_VALUE);

#if !POWER_MONITOR_INA219_COMPATIBLE
    if (PowerMonitor_ReadRegister(INA226_REG_MANUFACTURER_ID, &manufacturer_id) &&
        PowerMonitor_ReadRegister(INA226_REG_DEVICE_ID, &device_id))
    {
        sampling_data.manufacturer_id = manufacturer_id;
        sampling_data.device_id = device_id;
        sampling_data.identity_valid =
            (manufacturer_id == INA226_MANUFACTURER_ID) &&
            ((device_id & INA226_DEVICE_ID_MASK) == INA226_DEVICE_ID_VALUE);
    }
#endif

    verify_samples_remaining = POWER_MONITOR_VERIFY_INTERVAL;

    /* 寄存器事务成功即可继续采样，数值不一致仍通过状态字段显示。 */
    return true;
}

/* 周期回读工作寄存器，检测掉电复位或意外配置漂移。 */
static bool PowerMonitor_VerifyConfiguration(void)
{
    uint16_t config;
    uint16_t calibration;

    if (!PowerMonitor_ReadRegister(POWER_MONITOR_REG_CONFIG, &config) ||
        !PowerMonitor_ReadRegister(POWER_MONITOR_REG_CALIBRATION, &calibration))
    {
        PowerMonitor_MarkOffline();
        return false;
    }

    sampling_data.online = true;
    sampling_data.configuration_valid =
        (config == POWER_MONITOR_CONFIG_CONTINUOUS) &&
        (calibration == POWER_MONITOR_CALIBRATION_VALUE);
    verify_samples_remaining = POWER_MONITOR_VERIFY_INTERVAL;
    return true;
}

/*
 * 按所选芯片格式解码母线寄存器，再以四分之一毫伏为中间单位完成增益、
 * 偏移和四舍五入，避免 INA226 的 1.25 mV 分辨率被过早截断。
 */
static uint32_t PowerMonitor_ConvertBusVoltageMv(uint16_t raw_voltage)
{
#if POWER_MONITOR_INA219_COMPATIBLE
    /* INA219 的电压数据位于 bit15:3，最低三位为状态位，LSB=4 mV。 */
    uint32_t raw_quarter_mv = ((uint32_t)(raw_voltage >> 3U)) * 16UL;
#else
    /* INA226 使用完整无符号寄存器，LSB=1.25 mV。 */
    uint32_t raw_quarter_mv = (uint32_t)raw_voltage * 5UL;
#endif
    uint32_t scaled_quarter_mv =
        (raw_quarter_mv * POWER_MONITOR_BUS_GAIN_PERMILLE + 500UL) / 1000UL;
    int32_t corrected_quarter_mv =
        (int32_t)scaled_quarter_mv + POWER_MONITOR_BUS_OFFSET_MV * 4L;

    if (corrected_quarter_mv <= 0L)
    {
        return 0U;
    }

    return ((uint32_t)corrected_quarter_mv + 2UL) / 4UL;
}

/*
 * 将有符号分流电压直接换算为电流，绕过兼容器件可能异常的 Current（电流）
 * 寄存器。R010 表示 10 mΩ；换算结果保留电流方向，单位统一为 mA。
 */
static int32_t PowerMonitor_ConvertShuntCurrentMa(uint16_t raw_shunt_voltage)
{
    int32_t signed_shunt = (int32_t)(int16_t)raw_shunt_voltage;

#if POWER_MONITOR_INA219_COMPATIBLE
    /* INA219：R010 上 10 µV/bit 恰好对应 1 mA/bit。 */
    return signed_shunt;
#else
    /* INA226：R010 上 2.5 µV/bit 对应 0.25 mA/bit。 */
    return (signed_shunt >= 0L) ? ((signed_shunt + 2L) / 4L)
                               : ((signed_shunt - 2L) / 4L);
#endif
}

bool Sampling_Init(void)
{
    sampling_data = (sampling_data_t){0};
    sampling_data.ina219_compatible =
        POWER_MONITOR_INA219_COMPATIBLE != 0U;
    power_monitor_configured = PowerMonitor_Configure();
    sampling_data.data_valid = false;
    return power_monitor_configured;
}

void Sampling_Update(void)
{
    uint16_t raw_voltage;
    uint16_t raw_shunt_voltage;

    if (!power_monitor_configured)
    {
        if ((HAL_GetTick() - last_config_attempt_ms) < POWER_MONITOR_RETRY_INTERVAL_MS)
        {
            return;
        }

        power_monitor_configured = PowerMonitor_Configure();
        if (!power_monitor_configured)
        {
            return;
        }

        /* 等待下一次 200 ms 周期，使任一模式都有足够时间完成首次转换。 */
        return;
    }

    if (verify_samples_remaining == 0U)
    {
        if (!PowerMonitor_VerifyConfiguration())
        {
            return;
        }
    }
    else
    {
        verify_samples_remaining--;
    }

    if (!PowerMonitor_ReadRegister(POWER_MONITOR_REG_BUS_VOLTAGE, &raw_voltage) ||
        !PowerMonitor_ReadRegister(POWER_MONITOR_REG_SHUNT_VOLTAGE,
                                   &raw_shunt_voltage))
    {
        PowerMonitor_MarkOffline();
        return;
    }

    sampling_data.raw_bus_voltage = raw_voltage;
    sampling_data.input_voltage_mv = PowerMonitor_ConvertBusVoltageMv(raw_voltage);
    sampling_data.current_ma =
        PowerMonitor_ConvertShuntCurrentMa(raw_shunt_voltage);
    sampling_data.power_mw =
        ((int32_t)sampling_data.input_voltage_mv * sampling_data.current_ma) / 1000L;
    sampling_data.online = true;
    sampling_data.data_valid = true;
}

const sampling_data_t *Sampling_GetData(void)
{
    return &sampling_data;
}
