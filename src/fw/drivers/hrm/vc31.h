/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>

#include <stdbool.h>
#include <stdint.h>

//! Vcare VC31/VC31B optical heart-rate sensor (Bangle.js 2, I2C 0x33).
//!
//! INTERFACE-ONLY. The VC31 PPG->BPM conversion is a proprietary precompiled
//! blob (Espruino libs/misc/vc31_binary/); no open heart-rate algorithm exists.
//! This driver therefore only powers the part, verifies the WHO_AM_I, and can
//! read raw registers — it does NOT compute heart rate and never reports data up
//! to the HRM manager.

//! VC31 device-id (register 0x00) values (Espruino hrm_vc31.c).
#define VC31_WHO_AM_I_VC31A 0x11
#define VC31_WHO_AM_I_VC31B 0x21

typedef struct HRMDeviceState {
  bool enabled;
  bool initialized;
  uint8_t chip_id;  //!< WHO_AM_I read at init (0x11 VC31A / 0x21 VC31B)
} HRMDeviceState;

typedef const struct HRMDevice {
  HRMDeviceState *state;
  I2CSlavePort *i2c;
  OutputConfig en_gpio;     //!< EN power-enable pin (active high)
  InputConfig int_input;    //!< INT pin (data-ready; unused in interface-only mode)
} HRMDevice;
