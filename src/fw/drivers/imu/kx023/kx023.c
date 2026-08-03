/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// Kionix KX023-1025 accelerometer driver — POLLED.
//
// The Bangle.js 2 wires no accelerometer interrupt line (hardware-facts section 4),
// so this driver does not use EXTI/FIFO like lsm6dso. Instead a repeating NewTimer
// fires at the configured sampling interval, reads the six XOUT/YOUT/ZOUT output
// bytes over I2C, and hands a single scaled sample to accel_cb_new_sample() from
// the NewTimers thread context (accel.h permits calls from any thread context).
//
// Structure mirrors lsm6dso.c (WHO_AM_I check, ODR mapping, axis remap, mg scaling)
// minus the FIFO/INT machinery.

#include "board/board.h"
#include <pbl/drivers/accel.h>
#include <pbl/drivers/i2c.h>
#include <pbl/drivers/rtc.h>
#include "kernel/util/sleep.h"
#include "pbl/services/imu/units.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/util/math.h"
#include "system/logging.h"
#include "system/status_codes.h"

#include "kx023.h"
#include "registers.h"

PBL_LOG_MODULE_DEFINE(driver_accel_kx023, CONFIG_DRIVER_IMU_LOG_LEVEL);

// Time to wait after a software reset (ms).
#define KX023_RESET_TIME_MS 2

// Default sampling interval when none has been requested yet (50 Hz).
#define KX023_DEFAULT_INTERVAL_US 20000U

////////////////////////////////////////////////////////////////////////////////
// I2C helpers
////////////////////////////////////////////////////////////////////////////////

static bool prv_kx023_write(uint8_t reg, uint8_t value) {
  bool ret;
  i2c_use(&KX023->i2c);
  ret = i2c_write_register_block(&KX023->i2c, reg, 1, &value);
  i2c_release(&KX023->i2c);
  return ret;
}

static bool prv_kx023_read(uint8_t reg, uint8_t *data, uint16_t len) {
  bool ret;
  i2c_use(&KX023->i2c);
  ret = i2c_read_register_block(&KX023->i2c, reg, len, data);
  i2c_release(&KX023->i2c);
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// Sample conversion
////////////////////////////////////////////////////////////////////////////////

static int16_t prv_raw_to_s16(const uint8_t *raw) {
  return (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8U));
}

static int16_t prv_axis_raw_mg(IMUCoordinateAxis axis, const uint8_t *raw) {
  uint8_t offset = KX023->axis_map[axis];
  int16_t val = KX023->axis_dir[axis] *
                (int16_t)(((int32_t)prv_raw_to_s16(&raw[offset * 2U]) *
                           (int32_t)CONFIG_ACCEL_KX023_SCALE_MG) /
                          (int32_t)KX023_S16_SCALE_RANGE);

  if (KX023->state->rotated && (axis == AXIS_X || axis == AXIS_Y)) {
    val *= -1;
  }

  return val;
}

static void prv_raw_to_mg(const uint8_t *raw, AccelDriverSample *sample) {
  sample->x = prv_axis_raw_mg(AXIS_X, raw);
  sample->y = prv_axis_raw_mg(AXIS_Y, raw);
  sample->z = prv_axis_raw_mg(AXIS_Z, raw);
}

static uint64_t prv_now_us(void) {
  time_t time_s;
  uint16_t time_ms;
  rtc_get_time_ms(&time_s, &time_ms);
  return (((uint64_t)time_s) * 1000 + time_ms) * 1000ULL;
}

////////////////////////////////////////////////////////////////////////////////
// ODR / power configuration
////////////////////////////////////////////////////////////////////////////////

static uint8_t prv_odr_bits(uint32_t sampling_interval_us) {
  if (sampling_interval_us >= 80000UL) {
    return KX023_ODCNTL_OSA_12HZ5;
  } else if (sampling_interval_us >= 40000UL) {
    return KX023_ODCNTL_OSA_25HZ;
  } else if (sampling_interval_us >= 20000UL) {
    return KX023_ODCNTL_OSA_50HZ;
  } else if (sampling_interval_us >= 10000UL) {
    return KX023_ODCNTL_OSA_100HZ;
  } else {
    return KX023_ODCNTL_OSA_200HZ;
  }
}

static uint8_t prv_gsel_bits(void) {
  switch (CONFIG_ACCEL_KX023_SCALE_MG) {
    case 4000U:
      return KX023_CNTL1_GSEL_4G;
    case 8000U:
      return KX023_CNTL1_GSEL_8G;
    default:
      return KX023_CNTL1_GSEL_2G;
  }
}

// The KX023 must be in standby (PC1=0) to change ODR/range.
static bool prv_configure(uint32_t sampling_interval_us) {
  if (sampling_interval_us == 0U) {
    sampling_interval_us = KX023_DEFAULT_INTERVAL_US;
  }

  if (!prv_kx023_write(KX023_CNTL1, 0U)) {
    PBL_LOG_ERR("Could not enter standby");
    return false;
  }

  if (!prv_kx023_write(KX023_ODCNTL, prv_odr_bits(sampling_interval_us))) {
    PBL_LOG_ERR("Could not write ODCNTL");
    return false;
  }

  if (!prv_kx023_write(KX023_CNTL1, KX023_CNTL1_PC1 | KX023_CNTL1_RES | prv_gsel_bits())) {
    PBL_LOG_ERR("Could not enter operating mode");
    return false;
  }

  KX023->state->sampling_interval_us = sampling_interval_us;
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// Poll timer
////////////////////////////////////////////////////////////////////////////////

static void prv_poll_cb(void *data) {
  uint8_t raw[KX023_SAMPLE_SIZE_BYTES];

  if (!KX023->state->initialized || KX023->state->num_samples == 0U) {
    return;
  }

  if (!prv_kx023_read(KX023_XOUT_L, raw, sizeof(raw))) {
    PBL_LOG_ERR("Failed to read accel sample");
    return;
  }

  AccelDriverSample sample;
  prv_raw_to_mg(raw, &sample);
  sample.timestamp_us = prv_now_us();

  accel_cb_new_sample(&sample);
}

static uint32_t prv_interval_ms(void) {
  uint32_t ms = KX023->state->sampling_interval_us / 1000U;
  return MAX(ms, 1U);
}

static void prv_start_polling(void) {
  new_timer_start(KX023->state->poll_timer, prv_interval_ms(), prv_poll_cb, NULL,
                  TIMER_START_FLAG_REPEATING);
}

static void prv_stop_polling(void) {
  new_timer_stop(KX023->state->poll_timer);
}

////////////////////////////////////////////////////////////////////////////////
// Accelerometer interface
////////////////////////////////////////////////////////////////////////////////

void accel_init(void) {
  uint8_t val;

  if (!prv_kx023_read(KX023_WHO_AM_I, &val, 1)) {
    PBL_LOG_ERR("Could not read WHO_AM_I");
    return;
  }

  if (val != KX023_WHO_AM_I_VAL) {
    PBL_LOG_ERR("Unexpected id: 0x%02X!=0x%02X", val, KX023_WHO_AM_I_VAL);
    return;
  }

  // Software reset so we start from a known state.
  (void)prv_kx023_write(KX023_CNTL2, KX023_CNTL2_SRST);
  psleep(KX023_RESET_TIME_MS);

  if (!prv_configure(KX023_DEFAULT_INTERVAL_US)) {
    return;
  }

  KX023->state->poll_timer = new_timer_create();
  if (KX023->state->poll_timer == TIMER_INVALID_ID) {
    PBL_LOG_ERR("Could not create poll timer");
    return;
  }

  KX023->state->initialized = true;
  PBL_LOG_DBG("KX023 initialized");
}

uint32_t accel_set_sampling_interval(uint32_t interval_us) {
  if (!KX023->state->initialized) {
    KX023->state->sampling_interval_us = interval_us;
    return interval_us;
  }

  if (!prv_configure(interval_us)) {
    PBL_LOG_ERR("Could not reconfigure ODR");
  }

  // Reschedule an active poll at the new rate.
  if (KX023->state->num_samples > 0U) {
    prv_start_polling();
  }

  return KX023->state->sampling_interval_us;
}

uint32_t accel_get_sampling_interval(void) {
  return KX023->state->sampling_interval_us;
}

uint32_t accel_get_max_num_samples(void) {
  // Polled, one sample per timer fire — no hardware FIFO batching.
  return 1;
}

void accel_set_num_samples(uint32_t num_samples) {
  if (!KX023->state->initialized) {
    return;
  }

  KX023->state->num_samples = (uint16_t)MIN(num_samples, 1U);

  if (num_samples == 0U) {
    prv_stop_polling();
  } else {
    prv_start_polling();
  }
}

int accel_peek(AccelDriverSample *data) {
  uint8_t raw[KX023_SAMPLE_SIZE_BYTES];

  if (!KX023->state->initialized) {
    return E_ERROR;
  }

  if (!prv_kx023_read(KX023_XOUT_L, raw, sizeof(raw))) {
    PBL_LOG_ERR("Failed to read sample");
    return E_ERROR;
  }

  prv_raw_to_mg(raw, data);
  data->timestamp_us = prv_now_us();
  return 0;
}

void accel_enable_shake_detection(bool on) {
  // KX023 wake-up (WUFE) is not wired to an interrupt on the Bangle, so shake
  // detection would require software analysis of the polled stream. Not
  // implemented in the MVP polled driver.
  KX023->state->shake_detection_enabled = on;
  PBL_LOG_WRN("KX023 shake detection not implemented (polled)");
}

bool accel_get_shake_detection_enabled(void) {
  return KX023->state->shake_detection_enabled;
}

void accel_enable_double_tap_detection(bool on) {
  PBL_LOG_WRN("KX023 double-tap detection not implemented (polled)");
}

bool accel_get_double_tap_detection_enabled(void) {
  return false;
}

void accel_set_shake_sensitivity_high(bool sensitivity_high) {
}

void accel_set_shake_sensitivity_percent(uint8_t percent) {
}

void accel_set_rotated(bool rotated) {
  KX023->state->rotated = rotated;
  PBL_LOG_DBG("Set rotated state to %s", rotated ? "true" : "false");
}
