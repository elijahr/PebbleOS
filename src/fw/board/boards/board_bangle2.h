/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/drivers/backlight/pwm.h>
#include "drivers/imu/kx023/kx023.h"
#include <pbl/drivers/touch/cst816/touch_sensor_definitions.h>

extern const TouchSensor *CST816;

// Stage 2 sensors on software (bit-bang) I2C buses. The chip drivers reference
// these global slave-port symbols by name (drivers/pressure/bmp280.c etc.).
extern I2CSlavePort *const I2C_BMP280;
extern I2CSlavePort *const I2C_MAG;

// VC31 heart-rate sensor (interface-only) on the repurposed bit-bang HRM bus.
extern HRMDevice *const HRM;

// Magnetometer axis orientation. The Bangle.js 2 compass is an UNKNOWN part
// (Espruino UNKNOWN_0C); with no reference frame to correct, use the identity
// mapping (raw device axes straight through). No data-ready interrupt is wired.
static const BoardConfigMag BOARD_CONFIG_MAG = {
  .mag_config = {
    .axes_offsets = { [AXIS_X] = 0, [AXIS_Y] = 1, [AXIS_Z] = 2 },
    .axes_inverts = { [AXIS_X] = false, [AXIS_Y] = false, [AXIS_Z] = false },
  },
};

#define BT_VENDOR_ID 0x0EEA
#define BT_VENDOR_NAME "Core Devices LLC"

#define BOARD_LSE_MODE RCC_LSE_Bypass

#define BOARD_RTC_INST NRF_RTC1
#define BOARD_RTC_IRQN RTC1_IRQn

static const BoardConfig BOARD_CONFIG = {
  .backlight_on_percent = 25,
};

// Bangle.js 2 has a single physical button, BTN1 on P0.17. Espruino declares it
// IN_PULLDOWN but marks the pin NEGATED (BANGLEJS2.py:219): its HAL swaps the
// pull to PULLUP and inverts reads, so the effective hardware config is internal
// PULLUP, pressed = electrically LOW -- matching gfwilliams/pebble-banglejs2.
// PebbleOS assumes a four-button model; the three phantom slots are parked on
// GPIO_Pin_NULL (the nrf5 button drivers skip NULL pins) so a floating pad can
// never read as a stuck-pressed button under the pullup.
static const BoardConfigButton BOARD_CONFIG_BUTTON = {
    .buttons =
        {
            [BUTTON_ID_BACK] = {"Back",
                                {NRFX_GPIOTE_INSTANCE(0), 0, GPIO_Pin_NULL},
                                NRF_GPIO_PIN_PULLUP},
            [BUTTON_ID_UP] = {"Up",
                              {NRFX_GPIOTE_INSTANCE(0), 0, GPIO_Pin_NULL},
                              NRF_GPIO_PIN_PULLUP},
            [BUTTON_ID_SELECT] = {"Select",
                                  {NRFX_GPIOTE_INSTANCE(0), 4, NRF_GPIO_PIN_MAP(0, 17)},
                                  NRF_GPIO_PIN_PULLUP},
            [BUTTON_ID_DOWN] = {"Down",
                                {NRFX_GPIOTE_INSTANCE(0), 0, GPIO_Pin_NULL},
                                NRF_GPIO_PIN_PULLUP},
        },
    .active_high = false,
    // Only P0.17 (SELECT) is real. Time-disambiguate the one button: short press =
    // SELECT, long hold = BACK (our design choice; Gordon maps his to BACK).
    // Touch swipe -> UP/DOWN (CONFIG_TOUCH_NAV_BUTTONS) completes navigation.
    .select_short_back_long = true,
    .timer = NRFX_TIMER_INSTANCE(1),
};

// Non-PMIC power path (Bangle.js 2 has no PMIC). Battery voltage is sensed by a
// direct SAADC read on AIN1 (P0.03) and charge presence by the P0.23 GPIO
// (active low); both are handled inside drivers/battery/battery_bangle2.c.
static const BoardConfigPower BOARD_CONFIG_POWER = {
  .low_power_threshold = 2,
  .battery_capacity_hours = 168,
};

// Track C: vibration motor is a plain GPIO on P0.19 (active high, no haptic IC).
static const BoardConfigActuator BOARD_CONFIG_VIBE = {
  .ctl = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 19), true },
};

static const BoardConfigAccel BOARD_CONFIG_ACCEL = {
  .default_motion_sensitivity = 55U,  // Medium
};

extern UARTDevice * const DBG_UART;

// Backlight is GPIO P0.08 (software PWM on real hardware). Reuse the PWM
// backlight driver for the PRF skeleton; Track C reworks it to software PWM.
extern PwmState BACKLIGHT_PWM_STATE;
static const BacklightPwmConfig BACKLIGHT_PWM = {
  .ctl = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 8), true },
  .pwm = {
    .state = &BACKLIGHT_PWM_STATE,
    .output = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 8), true },
    .peripheral = NRFX_PWM_INSTANCE(0)
  },
  .max_duty_cycle_percent = 67,
};

// External 8 MB SPI NOR on the nRF52840 SPIM2 master. Pins are the real
// Bangle.js 2 flash bus (Espruino boards/BANGLEJS2.py): CS D14=P0.14,
// SCK D16=P0.16, MOSI/IO0 D15=P0.15, MISO/IO1 D13=P0.13. CS is a plain GPIO the
// spi_nor driver toggles per command (active low). 8 MHz matches the QSPI-era
// clock and the Espruino "too fast at 8 MHz" note applies to the panel bus, not
// this one. See drivers/flash/spi_nor.
static const BoardConfigFlashSPI BOARD_CONFIG_FLASH = {
  .spi = NRFX_SPIM_INSTANCE(2),

  .clk  = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 16), true },
  .mosi = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 15), true },
  .miso = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 13), true },
  // CS active low.
  .cs   = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 14), false },

  .clk_freq_hz = 8000000UL,
};

// LPM013M126 memory-in-pixel LCD on the nRF52840 SPIM3 bus.
//   SCK  P0.26, MOSI P0.27 (write-only panel, no MISO)
//   CS   P0.05 (active HIGH for 3/4bpp mode), DISP/enable P0.07 (high at init)
//   EXTCOMIN P0.06 (anti-burn-in VCOM; driven in hardware by RTC2+GPIOTE+PPI)
// Pins per boards/BANGLEJS2.py; encoding per Espruino libs/graphics/lcd_memlcd.c.
static const BoardConfigSharpDisplay BOARD_CONFIG_DISPLAY = {
  .spi = NRFX_SPIM_INSTANCE(3),

  .clk  = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 26), true },
  .mosi = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 27), true },
  .cs   = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 5),  true },

  .on_ctrl = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 7), true },
  .on_ctrl_otype = NRF_GPIO_PIN_S0S1,

  // EXTCOMIN (anti-burn-in VCOM) on P0.06. The display driver arms a free-running
  // RTC2 -> GPIOTE(ch6) -> PPI toggle at ~120 Hz (period_us) with a short pulse;
  // a static level here would physically damage the panel.
  .extcomin = {
    .rtc = NRF_RTC2,
    .gpiote = NRF_GPIOTE,
    .gpiote_ch = 6,
    .psel = NRF_GPIO_PIN_MAP(0, 6),
    .period_us = 1000000 / 120,
    .pulse_us = (1000000 / 120) / 20,
  },
};
