/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Generic I2C magnetometer driver for the Bangle.js 2 compass at I2C 0x0C.
//!
//! UNKNOWN PART. Espruino names this device "UNKNOWN_0C" (boards/BANGLEJS2.py)
//! and drives it with a reverse-engineered register map — there is no datasheet
//! and no verified part number. This driver reproduces only that access pattern
//! (libs/banglejs/jswrap_bangle.c):
//!   * presence = (read reg 0x00) != 0xFF;
//!   * a sample is 7 bytes at reg 0x4E: [status, Yhi, Ylo, Xhi, Xlo, Zhi, Zlo]
//!     (big-endian per axis, X/Y swapped, data-ready = status & 0x10);
//!   * reading reg 0x3E kicks the next conversion.
//! The returned counts are RAW and UNITLESS — the part's sensitivity is unknown,
//! so no milligauss scaling is applied (unlike a datasheeted part). The sensor
//! sits on a SOFTWARE (bit-bang) I2C bus reached through the standard i2c API.

#include <pbl/drivers/i2c.h>
#include <pbl/drivers/mag.h>
#include "board/board.h"
#include "pbl/os/mutex.h"
#include "system/logging.h"

#include <stdint.h>

PBL_LOG_MODULE_DEFINE(driver_mag_generic, CONFIG_DRIVER_IMU_LOG_LEVEL);

#define GENERIC_MAG_REG_PRESENCE  0x00
#define GENERIC_MAG_REG_KICK      0x3E
#define GENERIC_MAG_REG_SAMPLE    0x4E
#define GENERIC_MAG_SAMPLE_LEN    7
#define GENERIC_MAG_STATUS_DRDY   0x10
#define GENERIC_MAG_ABSENT        0xFF

typedef enum {
  X_AXIS = 0,
  Y_AXIS = 1,
  Z_AXIS = 2,
} axis_t;

static bool s_initialized;
static int s_use_refcount;
static PebbleMutex *s_mag_mutex;

static bool prv_read_block(uint8_t reg, uint8_t len, uint8_t *out) {
  i2c_use(I2C_MAG);
  bool rv = i2c_write_block(I2C_MAG, 1, &reg);
  if (rv) {
    rv = i2c_read_block(I2C_MAG, len, out);
  }
  i2c_release(I2C_MAG);
  return rv;
}

// Read one register (used to kick the next conversion, per Espruino).
static void prv_kick_conversion(void) {
  uint8_t discard = 0;
  (void)prv_read_block(GENERIC_MAG_REG_KICK, 1, &discard);
}

static int16_t prv_get_axis_projection(axis_t axis, const int16_t *raw_vector) {
  uint8_t axis_offset = BOARD_CONFIG_MAG.mag_config.axes_offsets[axis];
  bool invert = BOARD_CONFIG_MAG.mag_config.axes_inverts[axis];
  return (int16_t)((invert ? -1 : 1) * raw_vector[axis_offset]);
}

void mag_init(void) {
  if (s_initialized) {
    return;
  }
  s_mag_mutex = mutex_create();

  uint8_t id = 0;
  if (!prv_read_block(GENERIC_MAG_REG_PRESENCE, 1, &id) || id == GENERIC_MAG_ABSENT) {
    PBL_LOG_ERR("generic mag: not present (reg0x00=0x%02x)", id);
    return;
  }
  s_initialized = true;
  PBL_LOG_DBG("generic mag (UNKNOWN_0C): present");
}

void mag_use(void) {
  if (!s_initialized) {
    return;
  }
  mutex_lock(s_mag_mutex);
  if (s_use_refcount == 0) {
    prv_kick_conversion();  // power on / arm the first conversion
  }
  ++s_use_refcount;
  mutex_unlock(s_mag_mutex);
}

void mag_start_sampling(void) {
  mag_use();
  mag_change_sample_rate(MagSampleRate5Hz);
}

void mag_release(void) {
  if (!s_initialized) {
    return;
  }
  mutex_lock(s_mag_mutex);
  if (s_use_refcount > 0) {
    --s_use_refcount;
  }
  mutex_unlock(s_mag_mutex);
}

MagReadStatus mag_read_data(MagData *data) {
  if (!s_initialized) {
    return MagReadNoMag;
  }

  mutex_lock(s_mag_mutex);

  uint8_t raw[GENERIC_MAG_SAMPLE_LEN];
  if (!prv_read_block(GENERIC_MAG_REG_SAMPLE, sizeof(raw), raw)) {
    mutex_unlock(s_mag_mutex);
    return MagReadCommunicationFail;
  }

  if (!(raw[0] & GENERIC_MAG_STATUS_DRDY)) {
    mutex_unlock(s_mag_mutex);
    return MagReadCommunicationFail;
  }

  // Big-endian per axis, X/Y swapped in the register block (Espruino layout):
  //   Y = raw[1..2], X = raw[3..4], Z = raw[5..6].
  int16_t raw_vector[3];
  raw_vector[Y_AXIS] = (int16_t)(((uint16_t)raw[1] << 8) | raw[2]);
  raw_vector[X_AXIS] = (int16_t)(((uint16_t)raw[3] << 8) | raw[4]);
  raw_vector[Z_AXIS] = (int16_t)(((uint16_t)raw[5] << 8) | raw[6]);

  data->x = prv_get_axis_projection(X_AXIS, raw_vector);
  data->y = prv_get_axis_projection(Y_AXIS, raw_vector);
  data->z = prv_get_axis_projection(Z_AXIS, raw_vector);

  prv_kick_conversion();  // arm the next sample

  mutex_unlock(s_mag_mutex);
  return MagReadSuccess;
}

bool mag_change_sample_rate(MagSampleRate rate) {
  // The reverse-engineered part exposes no ODR register; the sample rate is a
  // software polling concern handled by the ecompass service. Accept both rates.
  (void)rate;
  return true;
}
