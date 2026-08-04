/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Bosch BMP280 barometric pressure + temperature driver.
//!
//! Fitted to the Bangle.js 2 at I2C 0x76 on a SOFTWARE (bit-bang) I2C bus — all
//! four nRF52840 hardware serial instances are consumed by display/flash/touch/
//! accel — reached through the same i2c_use / i2c_read_register_block API as a
//! hardware bus. Register map + fixed-point compensation follow the BMP280
//! datasheet (rev 1.19, §3.11.3 reference code); Espruino drives the same part
//! on this board (libs/banglejs/jswrap_bangle.c: id 0xD0==0x58, cal @ 0x88,
//! data @ 0xF7).

#include "board/board.h"
#include <pbl/drivers/pressure.h>
#include <pbl/drivers/i2c.h>
#include "pbl/util/logging.h"

#include <stdint.h>

PBL_LOG_MODULE_DEFINE(driver_pressure_bmp280, CONFIG_DRIVER_PRESSURE_LOG_LEVEL);

#define BMP280_REG_CHIP_ID     0xD0
#define BMP280_CHIP_ID_VALUE   0x58
#define BMP280_REG_RESET       0xE0
#define BMP280_REG_CTRL_MEAS   0xF4
#define BMP280_REG_CONFIG      0xF5
#define BMP280_REG_PRESS_MSB   0xF7
#define BMP280_REG_CALIB_START 0x88
#define BMP280_CALIB_LEN       24

// ctrl_meas: temperature oversampling x1 (0b001<<5), pressure oversampling x1
// (0b001<<2), normal mode (0b11). config: standby 0.5 ms, filter off.
#define BMP280_CTRL_MEAS_NORMAL 0x27
#define BMP280_CONFIG_DEFAULT   0xA0

// Calibration coefficients (BMP280 datasheet §3.11.2 layout at 0x88).
typedef struct {
  uint16_t dig_t1;
  int16_t dig_t2;
  int16_t dig_t3;
  uint16_t dig_p1;
  int16_t dig_p2;
  int16_t dig_p3;
  int16_t dig_p4;
  int16_t dig_p5;
  int16_t dig_p6;
  int16_t dig_p7;
  int16_t dig_p8;
  int16_t dig_p9;
} BMP280Calib;

static bool s_initialized;
static BMP280Calib s_calib;

static bool prv_read_register_block(uint8_t reg, uint8_t len, uint8_t *out) {
  i2c_use(I2C_BMP280);
  bool rv = i2c_write_block(I2C_BMP280, 1, &reg);
  if (rv) {
    rv = i2c_read_block(I2C_BMP280, len, out);
  }
  i2c_release(I2C_BMP280);
  return rv;
}

static bool prv_write_register(uint8_t reg, uint8_t datum) {
  i2c_use(I2C_BMP280);
  uint8_t d[2] = {reg, datum};
  bool rv = i2c_write_block(I2C_BMP280, 2, d);
  i2c_release(I2C_BMP280);
  return rv;
}

static uint16_t prv_u16le(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int16_t prv_s16le(const uint8_t *p) {
  return (int16_t)prv_u16le(p);
}

static bool prv_read_calibration(void) {
  uint8_t raw[BMP280_CALIB_LEN];
  if (!prv_read_register_block(BMP280_REG_CALIB_START, sizeof(raw), raw)) {
    return false;
  }
  s_calib.dig_t1 = prv_u16le(&raw[0]);
  s_calib.dig_t2 = prv_s16le(&raw[2]);
  s_calib.dig_t3 = prv_s16le(&raw[4]);
  s_calib.dig_p1 = prv_u16le(&raw[6]);
  s_calib.dig_p2 = prv_s16le(&raw[8]);
  s_calib.dig_p3 = prv_s16le(&raw[10]);
  s_calib.dig_p4 = prv_s16le(&raw[12]);
  s_calib.dig_p5 = prv_s16le(&raw[14]);
  s_calib.dig_p6 = prv_s16le(&raw[16]);
  s_calib.dig_p7 = prv_s16le(&raw[18]);
  s_calib.dig_p8 = prv_s16le(&raw[20]);
  s_calib.dig_p9 = prv_s16le(&raw[22]);
  return true;
}

void pressure_init(void) {
  if (s_initialized) {
    return;
  }

  uint8_t id = 0;
  if (!prv_read_register_block(BMP280_REG_CHIP_ID, 1, &id) || id != BMP280_CHIP_ID_VALUE) {
    PBL_LOG_ERR("BMP280 probe failed; id 0x%02x (want 0x%02x)", id, BMP280_CHIP_ID_VALUE);
    return;
  }

  if (!prv_read_calibration()) {
    PBL_LOG_ERR("BMP280 calibration read failed");
    return;
  }

  (void)prv_write_register(BMP280_REG_CONFIG, BMP280_CONFIG_DEFAULT);
  (void)prv_write_register(BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_NORMAL);

  s_initialized = true;
  PBL_LOG_DBG("BMP280 found; dig_T1=%u dig_P1=%u", s_calib.dig_t1, s_calib.dig_p1);
}

// BMP280 datasheet §3.11.3 32-bit temperature compensation. Returns temperature
// in centidegrees Celsius (5123 == 51.23 degC); publishes t_fine for pressure.
static int32_t prv_compensate_temperature(int32_t adc_t, int32_t *t_fine_out) {
  int32_t var1, var2, t;
  var1 = ((((adc_t >> 3) - ((int32_t)s_calib.dig_t1 << 1))) * ((int32_t)s_calib.dig_t2)) >> 11;
  var2 = (((((adc_t >> 4) - ((int32_t)s_calib.dig_t1)) *
            ((adc_t >> 4) - ((int32_t)s_calib.dig_t1))) >> 12) *
          ((int32_t)s_calib.dig_t3)) >> 14;
  int32_t t_fine = var1 + var2;
  *t_fine_out = t_fine;
  t = (t_fine * 5 + 128) >> 8;
  return t;
}

// BMP280 datasheet §3.11.3 64-bit pressure compensation. Returns pressure in Pa
// as an integer (the Q24.8 result shifted down to whole pascals).
static uint32_t prv_compensate_pressure(int32_t adc_p, int32_t t_fine) {
  int64_t var1, var2, p;
  var1 = ((int64_t)t_fine) - 128000;
  var2 = var1 * var1 * (int64_t)s_calib.dig_p6;
  var2 = var2 + ((var1 * (int64_t)s_calib.dig_p5) << 17);
  var2 = var2 + (((int64_t)s_calib.dig_p4) << 35);
  var1 = ((var1 * var1 * (int64_t)s_calib.dig_p3) >> 8) +
         ((var1 * (int64_t)s_calib.dig_p2) << 12);
  var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)s_calib.dig_p1) >> 33;
  if (var1 == 0) {
    return 0;  // avoid divide-by-zero
  }
  p = 1048576 - adc_p;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (((int64_t)s_calib.dig_p9) * (p >> 13) * (p >> 13)) >> 25;
  var2 = (((int64_t)s_calib.dig_p8) * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)s_calib.dig_p7) << 4);
  return (uint32_t)(p >> 8);  // Q24.8 -> whole pascals
}

bool pressure_read(int32_t *pressure_pa, int32_t *temperature_c) {
  if (!s_initialized) {
    return false;
  }

  uint8_t raw[6];
  if (!prv_read_register_block(BMP280_REG_PRESS_MSB, sizeof(raw), raw)) {
    return false;
  }

  int32_t adc_p = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | ((int32_t)raw[2] >> 4);
  int32_t adc_t = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | ((int32_t)raw[5] >> 4);

  int32_t t_fine = 0;
  int32_t t = prv_compensate_temperature(adc_t, &t_fine);
  uint32_t p = prv_compensate_pressure(adc_p, t_fine);

  if (temperature_c) {
    *temperature_c = t;
  }
  if (pressure_pa) {
    *pressure_pa = (int32_t)p;
  }
  return true;
}
