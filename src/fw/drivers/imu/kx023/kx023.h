/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/accel.h>
#include <pbl/drivers/i2c.h>
#include "pbl/services/new_timer/new_timer.h"

// KX023 sample size (X, Y, Z, 16-bit each).
#define KX023_SAMPLE_SIZE_BYTES 6

typedef struct KX023State {
  bool initialized;
  bool rotated;
  bool shake_detection_enabled;
  uint32_t sampling_interval_us;
  uint16_t num_samples;
  TimerID poll_timer;
  uint64_t next_timestamp_us;
} KX023State;

typedef struct KX023Config {
  //! Driver state.
  KX023State *state;
  //! I2C slave port (KX023 at 0x1E, polled — no interrupt line wired).
  I2CSlavePort i2c;
  //! Axis mapping (index by IMUCoordinateAxis -> raw axis 0:X 1:Y 2:Z).
  uint8_t axis_map[3];
  //! Axis direction (+1 or -1) per IMUCoordinateAxis.
  int8_t axis_dir[3];
} KX023Config;

extern const KX023Config *const KX023;
