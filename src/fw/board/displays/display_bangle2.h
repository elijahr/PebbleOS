/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// The system UI renders at the FLINT 144x168 mono geometry and the display
// driver letterboxes it onto the physical 176x176 LPM013M126 panel.
// LETTERBOX_OFFSET_{X,Y} are the single home for the centering offset; the
// display driver (S4) and the touch reverse-transform (Track B) both use them.
// (176 - 144) / 2 = 16, (176 - 168) / 2 = 4.
#define LETTERBOX_OFFSET_X 16
#define LETTERBOX_OFFSET_Y 4

// Physical panel dimensions (LPM013M126).
#define PANEL_DISPLAY_WIDTH 176
#define PANEL_DISPLAY_HEIGHT 176

#define DISPLAY_ORIENTATION_COLUMN_MAJOR_INVERTED 0
#define DISPLAY_ORIENTATION_ROTATED_180 1
#define DISPLAY_ORIENTATION_ROW_MAJOR 0
#define DISPLAY_ORIENTATION_ROW_MAJOR_INVERTED 0

#define PBL_BW 1
#define PBL_COLOR 0

#define PBL_RECT 1
#define PBL_ROUND 0

#define PBL_DISPLAY_WIDTH 144
#define PBL_DISPLAY_HEIGHT 168

#define LEGACY_2X_DISP_COLS PBL_DISPLAY_WIDTH
#define LEGACY_2X_DISP_ROWS PBL_DISPLAY_HEIGHT
#define LEGACY_3X_DISP_COLS PBL_DISPLAY_WIDTH
#define LEGACY_3X_DISP_ROWS PBL_DISPLAY_HEIGHT

#define DISPLAY_FRAMEBUFFER_BYTES \
    (ROUND_TO_MOD_CEIL(PBL_DISPLAY_WIDTH, 32) / 8 * PBL_DISPLAY_HEIGHT)
