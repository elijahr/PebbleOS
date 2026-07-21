/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "drivers/backlight/pwm.h"

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
