# vscode_g030_fan

基于 STM32G030F6P6 的裸机风扇控制器固件，使用旋转编码器或可选 ADC 电位器设置开环 PWM，占空比范围为 0–97%。项目同时监测风扇转速和 INA226 输入电源数据，并在 160×80 ST7735 TFT 上显示状态。

## 主要功能

- TIM16/PB8 输出 25 kHz 风扇 PWM。
- TIM17/PB9 持续监测风扇测速信号；测速只用于显示，不参与控制。
- 默认使用旋转编码器调速，支持快速旋转加速步长。
- 可通过 `FAN_DUTY_SOURCE` 切换为 PA5/ADC1_IN5 电位器调速。
- 编码器短按只切换 LED。
- INA226 通过 I2C2 采集母线电压和分流电压。
- ST7735 TFT 使用 SPI1，界面包含数值、状态和简单动画。
- 风扇占空比上升默认限制为约 2 秒，下降立即生效。

## 硬件

- MCU：STM32G030F6P6，Cortex-M0+，32 KiB Flash，8 KiB RAM
- 显示屏：160×80 ST7735
- 电源监控：INA226，默认 7 位 I²C 地址 `0x40`
- 分流电阻：R010（10 mΩ）
- 调试器：CMSIS-DAP

### 主要引脚

| 引脚 | 功能 |
|---|---|
| PA0 | TFT D/C |
| PA1 | SPI1 SCK |
| PA2 | SPI1 MOSI |
| PA3 | TFT CS |
| PA4 | LED 使能 |
| PA5 | ADC1_IN5，可选电位器输入 |
| PA6 / PA7 | TIM3 编码器 A/B |
| PA11 / PA12 | I2C2 SCL/SDA |
| PB0 | TFT 背光 |
| PB3 | TFT 复位 |
| PB8 | TIM16 风扇 PWM |
| PB9 | TIM17 风扇测速输入 |
| PC15 | 编码器按键，低电平有效 |

> **硬件安全：** PB8 是 MCU 的 3.3 V 逻辑输出，不能直接连接 12 V。两线或三线风扇的电源必须由外部 MOSFET/驱动电路控制；四线风扇 PWM 输入需要符合其接口规范。任何 12 V 回灌都可能损坏 MCU 和同一 3.3 V 电源域上的器件。

## 构建环境

项目使用 CMake Presets、Ninja 和 GNU Arm Embedded Toolchain。推荐版本与 `.settings/bundles.store.json` 一致：

- CMake 4.0.1+st.3
- Ninja 1.13.1+st.1
- GNU Tools for STM32 14.3.1+st.2

如果使用 STM32Cube VS Code 扩展提供的 bundle，请先设置环境变量。下面的路径只作格式示例，请替换为本机实际安装目录：

```sh
export CUBE_BUNDLE_PATH="C:/path/to/stm32cube/bundles"
export OPENOCD_HOME="C:/path/to/openocd"
export PATH="$CUBE_BUNDLE_PATH/cmake/4.0.1+st.3/bin:$CUBE_BUNDLE_PATH/ninja/1.13.1+st.1/bin:$CUBE_BUNDLE_PATH/gnu-tools-for-stm32/14.3.1+st.2/bin:$PATH"
```

`CUBE_BUNDLE_PATH` 和 `OPENOCD_HOME` 也供 `.vscode/tasks.json` 与 `.vscode/launch.json` 使用。应在启动 VS Code 前设置，或配置为操作系统用户环境变量。

## 编译

### Debug

```sh
cmake --preset Debug --fresh
cmake --build --preset Debug
```

### Release

```sh
cmake --preset Release --fresh
cmake --build --preset Release
```

每次成功构建都会生成：

```text
build/<preset>/vscode_g030_fan.elf
build/<preset>/vscode_g030_fan.hex
build/<preset>/vscode_g030_fan.bin
build/<preset>/vscode_g030_fan.map
```

仓库不提交 `build/` 或预编译固件，避免上传本机路径和过期产物。请从对应源码提交自行构建。

## 烧录与调试

OpenOCD 推荐烧录 ELF，因为 ELF 包含程序地址；烧录裸 BIN 时必须指定 Flash 基地址 `0x08000000`。

```sh
"$OPENOCD_HOME/bin/openocd.exe" \
  -s "$OPENOCD_HOME/share/openocd/scripts" \
  -f interface/cmsis-dap.cfg \
  -f target/stm32g0x.cfg \
  -c "program build/Release/vscode_g030_fan.elf verify reset exit"
```

VS Code 的 F5 配置使用 Debug ELF。烧录后调试器会停在 `main()`，需要再执行一次 Continue 才会继续初始化 GPIO、TFT 和其他外设。

## 调速来源

默认配置位于 `Code/function/control.h`：

```c
#define FAN_DUTY_SOURCE FAN_DUTY_SOURCE_ENCODER
```

需要使用 PA5 电位器时，将其切换为：

```c
#define FAN_DUTY_SOURCE FAN_DUTY_SOURCE_ADC
```

切换后应执行一次 `--fresh` 构建，避免旧 CMake 缓存影响结果。

## 目录结构

- `Core/`：STM32CubeMX 生成的启动和外设代码
- `Drivers/`：STM32 HAL/LL 与 CMSIS
- `Code/function/`：控制、风扇、编码器、ADC 和 INA226 逻辑
- `Code/show/`：TFT 驱动、字库和界面
- `cmake/`：工具链、CubeMX 与用户源码集成
- `vscode_g030_fan.ioc`：CubeMX 工程配置

## 验证范围

项目没有自动测试套件。提交前的基础验证是 Debug/Release 无警告构建、固件产物检查以及实机测试。编译成功不代表受损或接线错误的硬件可以正常工作。

## 第三方代码

`Drivers/` 中的 STM32 HAL/LL 与 CMSIS 文件继续遵循其目录内的 STMicroelectronics/Arm 许可证。本仓库未为项目自有代码额外声明开源许可证。
