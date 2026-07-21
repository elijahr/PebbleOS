/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// GPIO vibration-motor driver for the Bangle.js 2. The motor is driven directly
// by a single GPIO (P0.19, active high) with no haptic controller, so strength
// collapses to on/off.

#include <stdlib.h>

#include "board/board.h"
#include "console/prompt.h"
#include "drivers/gpio.h"
#include "drivers/vibe.h"

static bool s_enabled;

void vibe_init(void) {
  gpio_output_init(&BOARD_CONFIG_VIBE.ctl, GPIO_OType_PP);
  gpio_output_set(&BOARD_CONFIG_VIBE.ctl, false);
  s_enabled = false;
}

void vibe_ctl(bool on) {
  gpio_output_set(&BOARD_CONFIG_VIBE.ctl, on);
  s_enabled = on;
}

void vibe_force_off(void) {
  vibe_ctl(false);
}

void vibe_set_strength(int8_t strength) {
  // GPIO motor: no proportional drive, map non-zero strength to full on.
  vibe_ctl(strength != VIBE_STRENGTH_OFF);
}

int8_t vibe_get_braking_strength(void) {
  return VIBE_STRENGTH_OFF;
}

status_t vibe_calibrate(void) {
  return E_INVALID_OPERATION;
}

uint8_t vibe_get_calibration(void) {
  return 0xFF;
}

void vibe_apply_calibration(uint8_t cali) {
}

void command_vibe_ctl(const char *arg) {
  int strength = atoi(arg);

  const bool out_of_bounds = ((strength < 0) || (strength > VIBE_STRENGTH_MAX));
  const bool not_a_number = (strength == 0 && arg[0] != '0');
  if (out_of_bounds || not_a_number) {
    prompt_send_response("Invalid argument");
    return;
  }

  vibe_set_strength(strength);
  vibe_ctl(strength != 0);
  prompt_send_response("OK");
}
