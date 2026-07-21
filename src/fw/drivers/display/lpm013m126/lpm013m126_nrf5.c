/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// No-op skeleton driver for the Bangle.js 2 LPM013M126 memory-in-pixel LCD.
//
// This is the S2 pre-stub: it satisfies the display driver interface so the
// PRF firmware links, but it drives no hardware. S4 replaces the body with the
// real SPIM3 / letterbox / 3bpp-packing implementation (modeled on
// sharp_ls013b7dh01_nrf5.c, encoding per Espruino's lcd_memlcd.c).

#include "drivers/display/display.h"

#include <stdbool.h>
#include <stdint.h>

void display_init(void) {}

void display_clear(void) {}

void display_set_enabled(bool enabled) {}

void display_set_rotated(bool rotated) {}

void display_update(NextRowCallback nrcb, UpdateCompleteCallback uccb) {
  // Drain the rows the compositor offers so its framebuffer bookkeeping stays
  // consistent, then signal completion. No pixels are pushed to hardware yet.
  DisplayRow row;
  while (nrcb(&row)) {
  }
  if (uccb) {
    uccb();
  }
}

bool display_update_in_progress(void) { return false; }

void display_update_boot_frame(uint8_t *framebuffer) {}
