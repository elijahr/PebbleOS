/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

// Panel geometry + letterbox offsets (LETTERBOX_OFFSET_{X,Y},
// PANEL_DISPLAY_{WIDTH,HEIGHT}, PBL_DISPLAY_{WIDTH,HEIGHT}). This header is
// bangle2-specific: the affine below was measured on the bangle2 panel.
#include "board/displays/display_bangle2.h"

// Shared chip->physical de-shear core for the bangle2 CST816D (pure integer
// math; no driver deps, so host tests compile it standalone).
//
// The measured phys->chip affine maps unit physical strokes to chip deltas:
//   phys_x -> chip(-105, +88),  phys_y -> chip(+230, +87)
// Its integer inverse (det = (-105)(87) - (230)(88) = -29375):
//   px = 87*dx - 230*dy;    // = phys_x * det
//   py = -88*dx - 105*dy;   // = phys_y * det
// det < 0, so the true physical signs are the NEGATED signs of px/py; the
// shared |det| factor cancels in ratio tests (gesture classification).
#define CST816_DESHEAR_PX_DX (87)
#define CST816_DESHEAR_PX_DY (-230)
#define CST816_DESHEAR_PY_DX (-88)
#define CST816_DESHEAR_PY_DY (-105)

static inline int32_t cst816_deshear_px(int32_t dx, int32_t dy) {
  return CST816_DESHEAR_PX_DX * dx + CST816_DESHEAR_PX_DY * dy;
}

static inline int32_t cst816_deshear_py(int32_t dx, int32_t dy) {
  return CST816_DESHEAR_PY_DX * dx + CST816_DESHEAR_PY_DY * dy;
}

// Physical coordinates with conventional signs (+x RIGHT, +y DOWN): the
// negation of the de-shear rows (det < 0):
//   phys_x = 230*cy - 87*cx;   phys_y = 88*cx + 105*cy;
// Self-check (zero shear bleed, exact): the pure physical-vertical chip
// direction (+230, +87) gives delta_phys_x = 230*87 - 87*230 = 0.
static inline int32_t cst816_phys_x(int32_t cx, int32_t cy) {
  return -cst816_deshear_px(cx, cy);
}

static inline int32_t cst816_phys_y(int32_t cx, int32_t cy) {
  return -cst816_deshear_py(cx, cy);
}

// Measured chip coordinate ranges (SWD touchlog captures; see the recognizer
// header comment in cst816.c).
#define CST816_CHIP_X_MIN (1)
#define CST816_CHIP_X_MAX (238)
#define CST816_CHIP_Y_MIN (27)
#define CST816_CHIP_Y_MAX (256)

// Physical-range corners, derived from the rows + measured chip ranges so the
// derivation is auditable here. phys_x falls with cx and rises with cy;
// phys_y rises with both.
#define CST816_PHYS_X_MIN \
  (-CST816_DESHEAR_PX_DX * CST816_CHIP_X_MAX - CST816_DESHEAR_PX_DY * CST816_CHIP_Y_MIN)  // -14496
#define CST816_PHYS_X_MAX \
  (-CST816_DESHEAR_PX_DX * CST816_CHIP_X_MIN - CST816_DESHEAR_PX_DY * CST816_CHIP_Y_MAX)  // 58793
#define CST816_PHYS_X_SPAN (CST816_PHYS_X_MAX - CST816_PHYS_X_MIN)                        // 73289
#define CST816_PHYS_Y_MIN \
  (-CST816_DESHEAR_PY_DX * CST816_CHIP_X_MIN - CST816_DESHEAR_PY_DY * CST816_CHIP_Y_MIN)  // 2923
#define CST816_PHYS_Y_MAX \
  (-CST816_DESHEAR_PY_DX * CST816_CHIP_X_MAX - CST816_DESHEAR_PY_DY * CST816_CHIP_Y_MAX)  // 47824
#define CST816_PHYS_Y_SPAN (CST816_PHYS_Y_MAX - CST816_PHYS_Y_MIN)                        // 44901

// Chip -> framebuffer: scale the physical range onto the 176x176 panel with
// round-half-up, reverse the display letterbox offset, clamp to the
// framebuffer. The linear part is what drag deltas consume; the OFFSET term
// is provisional until the S9 two-point silicon gate (<= 4 fb px per axis).
// Overflow: decoded chip coords are 12-bit, so |phys| <= 230*4095 < 1e6 and
// every intermediate (x176) stays well inside int32.
//   fb_x = round((phys_x - MIN) * 176 / SPAN) - LETTERBOX_OFFSET_X, clamp [0,143]
//   fb_y = round((phys_y - MIN) * 176 / SPAN) - LETTERBOX_OFFSET_Y, clamp [0,167]
static inline int16_t cst816_transform_fb_x(int32_t cx, int32_t cy) {
  const int32_t rel = cst816_phys_x(cx, cy) - CST816_PHYS_X_MIN;
  const int32_t panel = (rel * PANEL_DISPLAY_WIDTH + CST816_PHYS_X_SPAN / 2) / CST816_PHYS_X_SPAN;
  int32_t fb = panel - LETTERBOX_OFFSET_X;
  if (fb < 0) {
    fb = 0;
  } else if (fb > PBL_DISPLAY_WIDTH - 1) {
    fb = PBL_DISPLAY_WIDTH - 1;
  }
  return (int16_t)fb;
}

static inline int16_t cst816_transform_fb_y(int32_t cx, int32_t cy) {
  const int32_t rel = cst816_phys_y(cx, cy) - CST816_PHYS_Y_MIN;
  const int32_t panel = (rel * PANEL_DISPLAY_HEIGHT + CST816_PHYS_Y_SPAN / 2) / CST816_PHYS_Y_SPAN;
  int32_t fb = panel - LETTERBOX_OFFSET_Y;
  if (fb < 0) {
    fb = 0;
  } else if (fb > PBL_DISPLAY_HEIGHT - 1) {
    fb = PBL_DISPLAY_HEIGHT - 1;
  }
  return (int16_t)fb;
}
