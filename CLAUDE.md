# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and flash

This is a bare-metal STM32CubeMX C11 project for an STM32G030F6P6 (Cortex-M0+, 32 KiB flash, 8 KiB RAM). CMake presets use Ninja and the GNU Arm Embedded toolchain. On this machine the STM32Cube bundle tools are not always on `PATH`; initialize the shell with:

```sh
export CUBE_BUNDLE_PATH="/path/to/stm32cube/bundles"
export PATH="$CUBE_BUNDLE_PATH/cmake/4.0.1+st.3/bin:$CUBE_BUNDLE_PATH/ninja/1.13.1+st.1/bin:$CUBE_BUNDLE_PATH/gnu-tools-for-stm32/14.3.1+st.2/bin:$PATH"
```

Configure and build:

```sh
# Fresh Debug build
cmake --preset Debug --fresh
cmake --build --preset Debug

# Fresh Release build (preferred for hardware)
cmake --preset Release --fresh
cmake --build --preset Release

# Incremental rebuild after the initial configure
cmake --build --preset Release
```

The firmware images are `build/<preset>/vscode_g030_fan.elf`, `.hex`, and `.bin`; the link map is `build/<preset>/vscode_g030_fan.map`. Every successful build runs the `firmware_images` target and regenerates HEX and raw BIN from the latest ELF, even when no source file needed recompilation. Use ELF for OpenOCD/Cortex-Debug because it carries addresses and debug symbols; a tool that programs the raw BIN must use flash base address `0x08000000`. The linker prints flash/RAM usage after every link. In the current default encoder configuration, Debug uses about 79.5% flash and Release about 55.4%; both use about 25.7% RAM. Release remains preferred for hardware. Application code retains `-O0 -g3` in Debug, while the vendor `STM32_Drivers` object library is compiled with `-Os` in Debug to keep both input variants inside the 32 KiB device.

There is no automated test suite, CTest target, separate lint target, simulator, or single-test command. Validation is a warning-free Debug/Release build followed by hardware testing. Compilation already uses `-Wall`; Release uses `-Os`; in Debug, application sources use `-O0 -g3` and the vendor HAL/LL driver objects use `-Os`.

Flash the Release ELF with the configured CMSIS-DAP probe and OpenOCD:

```sh
"$OPENOCD_HOME/bin/openocd.exe" \
  -s "$OPENOCD_HOME/share/openocd/scripts" \
  -f interface/cmsis-dap.cfg \
  -f target/stm32g0x.cfg \
  -c "program build/Release/vscode_g030_fan.elf verify reset exit"
```

`.vscode/launch.json` provides Cortex-Debug/OpenOCD debugging and points explicitly to `build/Debug/vscode_g030_fan.elf`. Its `preLaunchTask` configures and builds the Debug preset before every F5 launch, so F5 always flashes the current debuggable image. It has `runToEntryPoint: main`, so the target intentionally halts at `main()` after programming; continue once more to run the firmware. `Ctrl+Shift+B` also invokes the default Debug build task; when using the CMake Tools status-bar Build button directly, keep its active preset set to Debug.

## Source ownership and generation

- `vscode_g030_fan.ioc` is the intended CubeMX peripheral/pin configuration.
- `Core/` and `cmake/stm32cubemx/` are CubeMX-generated. Put durable manual changes inside `/* USER CODE BEGIN ... */` blocks when possible and keep the `.ioc` synchronized with changes to generated peripheral fields.
- `Code/function/` and `Code/show/` are the handwritten application and display layers. Their source list is maintained in `cmake/user/CMakeLists.txt`.
- `Drivers/` is the vendored STM32 HAL/LL and CMSIS code; do not implement application behavior there.

After regenerating from CubeMX, explicitly recheck these integration points before trusting a build:

1. `main.c` still initializes the TFT before `Control_Init()`, so fan tach interrupts cannot starve display reset/delays.
2. `main.c` still calls `MX_ADC1_Init()` only when `FAN_DUTY_SOURCE == FAN_DUTY_SOURCE_ADC`; an unconditional Cube-generated call unnecessarily pulls the ADC HAL into encoder-only builds.
3. `stm32g0xx_it.c` still calls `Control_NotifyTimerInterrupt()` from TIM14 and `Fan_HandleTimerInterrupt()` from TIM17. An enabled TIM17 IRQ without the latter causes a permanent interrupt storm because capture/update flags remain set.
4. SPI1 remains compatible with the ST7735 display and the `.ioc`: Mode 0, 8 MHz, PA1/PA2 high-speed GPIO in the current intended configuration.
5. TIM17 input capture filtering remains synchronized. The `.ioc` currently requests filter value 15 (`FDIV32_N8`); Cube-generated `tim.c` has previously drifted to `FDIV1` (no filter), which makes PB9 tach noise much more expensive.
6. TIM16/PB8 remains PWM1, active-high, 25 kHz, AF2, very-high-speed, no-pull and push-pull in the generated configuration. `Fan_Init()` changes only the output type when `FAN_PWM_DRIVE_MODE` explicitly selects open-drain.
7. PA5 remains ADC1_IN5 with the 160.5-cycle common sample time, and PC15 remains the active-low encoder button configured as a pulled-up GPIO input.
8. On the current schematic, physical pin 6 (NRST) is net-labelled `INPUT`. It must idle near 3.3 V and must never be connected to a 5/12 V power input; a low NRST holds the MCU in reset and leaves the display dark.

Treat the generated C actually compiled by CMake as runtime truth, but fix the `.ioc` as well so regeneration does not undo a hardware correction.

## Runtime architecture

`Core/Src/main.c` initializes HAL, the 64 MHz clock and peripherals, then calls `Show_Init()`, `Control_Init()`, and the initial `Show_Update()`. ADC1 is initialized only in the ADC-controlled build. TIM14 is enabled only after this startup sequence. The foreground loop has no RTOS: it repeatedly calls `Control_Process()` and redraws only when that function reports changed state.

`Code/function/control.c` is the application coordinator and the sole state presented to the UI through `control_state_t`:

- The TIM14 ISR supplies one 10 ms tick and scans the encoder/button through `Control_NotifyTimerInterrupt()`.
- The ISR only accumulates compact input/tick state. `Control_Process()` atomically consumes it in the foreground.
- Fan control runs every 100 ms; power-monitor sampling runs every 200 ms.
- `FAN_DUTY_SOURCE` is the compile-time target source selector. The default `FAN_DUTY_SOURCE_ENCODER` uses the rotary encoder. `FAN_DUTY_SOURCE_ADC` instead samples a potentiometer on PA5/ADC1_IN5; encoder rotation is ignored in this mode, but its push button still controls the LED.
- A short button press only toggles the LED. There is no independent FAN enable: a target of 0% means stopped and any target above 0% means running.
- In encoder mode, same-direction detents within `ENCODER_ACCEL_WINDOW_MS` (150 ms by default) accumulate into a burst and use 1%, 5%, 10%, then 20% per detent. A pause or direction reversal resets the burst to the 1% step.
- Fan control is open loop only: the target range is 0–97%. Measured RPM is independent monitoring data and never changes output duty.
- I2C, SPI, and ADC work must remain outside interrupts. Do not call `Show_Update()`, `Sampling_Update()`, `AdcInput_ReadDutyPercent()`, or other blocking HAL transfers from an ISR.

`Code/function/key.c` adapts TIM3 X4 quadrature counts and the active-low PC15 push button into detent/short-press events. It assumes four timer counts per mechanical detent and performs 30 ms button debouncing on the 10 ms scheduler tick. The short-press event is emitted after a stable press is released. `KEY_ENCODER_DIRECTION` selects the count sign; the current hardware default is `KEY_ENCODER_DIRECTION_REVERSED`, so clockwise rotation increases the target.

`Code/function/adc_input.c` owns the optional potentiometer input. It calibrates ADC1 once, performs software-triggered foreground conversions with a 2 ms poll timeout, applies a one-pole integer EMA (new sample weight 1/4 by default), and maps raw values above `ADC_INPUT_DEADBAND_RAW` (32 counts by default) linearly to 0–97%. Conversion failures retain the last valid target; calibration failure keeps the target at 0%.

`Code/function/fan.c` owns all fan control state:

- Fan output is open loop only. The selected source sets `target_duty_percent` directly from 0–97%; there is no control-mode selector, independent enable, or target-RPM state, and feedback never changes requested duty.
- `FAN_PWM_DRIVE_MODE` selects PB8 push-pull or open-drain drive. The current default is push-pull, matching the `.ioc` and external MOSFET/gate-driver inputs. Select open-drain only for a four-wire fan PWM input with a suitable external pull-up. PB8 is always a logic output and cannot directly power a two- or three-wire fan.
- TIM17 CH1/PB9 is always a 1 MHz rising-edge tach capture. It extends the 16-bit timer with update overflows, assumes two pulses per revolution, times out after 500 ms, and rejects periods below 1000 us. The measured RPM is used only for display and animation; it never affects PWM output. No signal is a normal two-wire-fan condition and produces a displayed speed of 0 RPM.
- `Fan_Process()` applies the percentage target, then passes increases through the common upward slew limiter. `FAN_DUTY_RAMP_UP_TIME_MS` defaults to 2000 ms, and output is capped at 970 permille because the current target maximum is 97%. Duty decreases, including a target of zero, take effect immediately at the next 100 ms fan task. Keep ISR work in `Fan_HandleTimerInterrupt()` limited to timestamp/flag handling.

`Code/function/Sampling.c` owns the standard INA226 power monitor at I2C address `0x40` (HAL receives the left-shifted address) and uses integer engineering units: mV, mA and mW. The INA219-compatible path remains available only as a compile-time fallback:

- `POWER_MONITOR_INA219_COMPATIBLE` is the single compile-time selector and now defaults to `0`. Standard INA226 mode uses configuration `0x4527`, calibration `0x0800`, full-register 1.25 mV/bit bus decoding, and identity registers `0xFE`/`0xFF`. Override it to `1` only for the old compatible part (`0x399F`, `0x4000`, `(raw >> 3) * 4 mV`).
- Both paths assume an R010 (10 mOhm) shunt. INA226 shunt voltage is 2.5 uV/bit, which becomes 0.25 mA/bit; current is derived directly from the signed shunt register rather than the device Current register.
- Configuration/identity mismatches are diagnostics and do not block otherwise successful samples; I2C failures mark data offline and trigger periodic reconfiguration.
- Board voltage correction is centralized in `POWER_MONITOR_BUS_GAIN_PERMILLE` and `POWER_MONITOR_BUS_OFFSET_MV`. Genuine INA226 mode starts at 1000 permille. Preserve integer math and calibrate gain as `reference_mV * 1000 / displayed_mV` only after register-format decoding is correct.
- Some public state fields retain `ina226_*` or `ina219_compatible` names for display compatibility; do not infer the active register format from names alone.

## Display architecture and constraints

`Code/show/tft.c` is the blocking SPI graphics driver; `Code/show/show.c` renders the 160x80 dashboard from `Control_GetState()` and caches previously displayed values to avoid redundant redraws. There is no framebuffer. Rendering uses small row buffers on the stack, which matters because the linker reserves only 0x400 bytes for stack.

- Keep `TFT_ORIENTATION` at `2U` unless the physical mounting or all layout coordinates are intentionally changed.
- The small 6x8 font contains only characters from space through uppercase `Z`. The sparse 12x24 font contains only space, minus, decimal point and digits. UI text must stay within these sets or unsupported characters render as spaces.
- Electrical values and RPM use the sparse large font; 16x16 monochrome frames implement FAN and LED animations.
- `Show_Init()` draws static regions. The layout uses a 0–9 top bar, 11–44 measurement cards, and a 46–79 fan/RPM/details/animation region; there are no bottom menu cells. `Show_Update()` handles INA status, measurements, RPM, target/applied PWM, run state, and animations. Its cache comparisons must be updated whenever a field affecting a region is added to `control_state_t`.
- FAN status is `STOP` when the applied PWM is zero and `RUN` when it is nonzero. The measured RPM region always shows a five-digit-capable numeric value and falls to `0` when PB9 has no valid signal. Animation uses measured motion when tach is available and actual PWM as the fallback when no tach is available.
- Status priority is offline (`INA ERR`), configuration mismatch (`INA CFG`), INA219-compatible (`INA219`), identity mismatch (`INA ID`), then valid INA226 (`INA OK`). Measurement visibility depends on online/data-valid state, not ID validity.

## Hardware map

The important cross-module pin assignments are:

| Pin | Function |
|---|---|
| NRST (physical pin 6) | Net `INPUT` on the current schematic; must idle high near 3.3 V and remain within MCU voltage limits |
| PA0 | TFT D/C |
| PA1 | SPI1 SCK |
| PA2 | SPI1 MOSI |
| PA3 | TFT CS |
| PA4 | LED enable |
| PA5 | ADC1_IN5 potentiometer input for optional ADC duty control |
| PA6 / PA7 | TIM3 encoder channels A/B |
| PA11 / PA12 | I2C2 SCL/SDA, 100 kHz with pull-ups |
| PB0 | TFT backlight |
| PB3 | TFT reset |
| PB8 | TIM16 fan PWM, push-pull by default for an external power driver; optional open-drain for a pulled-up four-wire PWM input |
| PB9 | TIM17 fan tach input with pull-up; measured continuously for display only and never used to control PWM |
| PC15 | Encoder push button, active low with an internal pull-up; short press toggles LED |

The display and all control updates share a single foreground execution context. Preserve that model unless introducing an explicit concurrency design; it is what makes module-owned state and the display cache safe without locks.
