/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "drivers/backlight/pwm.h"
#include "drivers/imu/kx023/kx023.h"
#include "drivers/touch/cst816/touch_sensor_definitions.h"

extern const TouchSensor *CST816;

#define BT_VENDOR_ID 0x0EEA
#define BT_VENDOR_NAME "Core Devices LLC"

#define BOARD_LSE_MODE RCC_LSE_Bypass

#define BOARD_RTC_INST NRF_RTC1
#define BOARD_RTC_IRQN RTC1_IRQn

static const BoardConfig BOARD_CONFIG = {
  .backlight_on_percent = 25,
};

// Bangle.js 2 has a single physical button (BTN1 on P0.17, IN_PULLDOWN, active
// high). PebbleOS assumes a four-button model, so the remaining slots point at
// unused GPIOs as placeholders; Track B (O-4) finalizes the single-button +
// touch mapping onto Pebble's button model.
static const BoardConfigButton BOARD_CONFIG_BUTTON = {
  .buttons = {
    [BUTTON_ID_BACK] =
        { "Back",   { NRFX_GPIOTE_INSTANCE(0), 2, NRF_GPIO_PIN_MAP(0, 10) }, NRF_GPIO_PIN_PULLDOWN },
    [BUTTON_ID_UP] =
        { "Up",     { NRFX_GPIOTE_INSTANCE(0), 3, NRF_GPIO_PIN_MAP(0, 4)  }, NRF_GPIO_PIN_PULLDOWN },
    [BUTTON_ID_SELECT] =
        { "Select", { NRFX_GPIOTE_INSTANCE(0), 4, NRF_GPIO_PIN_MAP(0, 17) }, NRF_GPIO_PIN_PULLDOWN },
    [BUTTON_ID_DOWN] =
        { "Down",   { NRFX_GPIOTE_INSTANCE(0), 5, NRF_GPIO_PIN_MAP(0, 28) }, NRF_GPIO_PIN_PULLDOWN },
  },
  .active_high = true,
  .timer = NRFX_TIMER_INSTANCE(1),
};

// Non-PMIC power path (Bangle.js 2 has no PMIC). Battery sense and charge
// detect are wired in Track C; this slot keeps the core power service linkable.
static const BoardConfigPower BOARD_CONFIG_POWER = {
  .low_power_threshold = 2,
  .battery_capacity_hours = 168,
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

extern QSPIPort * const QSPI;
extern QSPIFlash * const QSPI_FLASH;

// LPM013M126 memory-in-pixel LCD on the nRF52840 SPIM3 bus.
//   SCK  P0.26, MOSI P0.27 (write-only panel, no MISO)
//   CS   P0.05 (active HIGH for 3/4bpp mode), DISP/enable P0.07 (high at init)
//   EXTCOMIN P0.06 (anti-burn-in; optional for the Renode MVP, not driven yet)
// Pins per boards/BANGLEJS2.py; encoding per Espruino libs/graphics/lcd_memlcd.c.
static const BoardConfigSharpDisplay BOARD_CONFIG_DISPLAY = {
  .spi = NRFX_SPIM_INSTANCE(3),

  .clk  = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 26), true },
  .mosi = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 27), true },
  .cs   = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 5),  true },

  .on_ctrl = { NRF5_GPIO_RESOURCE_EXISTS, NRF_GPIO_PIN_MAP(0, 7), true },
  .on_ctrl_otype = NRF_GPIO_PIN_S0S1,

  // EXTCOMIN slot kept for hardware-correctness; the MVP driver does not start
  // the RTC/PPI toggle (Track C reworks the anti-burn-in path).
  .extcomin = {
    .rtc = NRF_RTC2,
    .gpiote = NRF_GPIOTE,
    .gpiote_ch = 6,
    .psel = NRF_GPIO_PIN_MAP(0, 6),
    .period_us = 1000000 / 120,
    .pulse_us = (1000000 / 120) / 20,
  },
};
