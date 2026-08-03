/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Vcare VC31/VC31B optical heart-rate driver (Bangle.js 2, I2C 0x33).
//!
//! INTERFACE-ONLY. The VC31 PPG->BPM conversion is a proprietary precompiled
//! blob (Espruino libs/misc/vc31_binary/); no open heart-rate algorithm exists.
//! This driver powers the part via its EN pin, verifies the WHO_AM_I (register
//! 0x00: 0x11 = VC31A, 0x21 = VC31B), and can read raw registers over the
//! SOFTWARE (bit-bang) I2C bus — it does NOT compute heart rate and never calls
//! hrm_manager_new_data_cb().

#include <pbl/drivers/hrm.h>
#include "drivers/hrm/vc31.h"
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/i2c.h>
#include "pbl/util/logging.h"

#include <stdint.h>

PBL_LOG_MODULE_DEFINE(driver_hrm_vc31, CONFIG_DRIVER_HRM_LOG_LEVEL);

#define VC31_REG_WHO_AM_I 0x00

static bool prv_read_whoami(HRMDevice *dev, uint8_t *out) {
  i2c_use(dev->i2c);
  bool rv = i2c_read_register(dev->i2c, VC31_REG_WHO_AM_I, out);
  i2c_release(dev->i2c);
  return rv;
}

void hrm_init(HRMDevice *dev) {
  if (dev->state->initialized) {
    return;
  }

  // Power the sensor and park INT as an input (no EXTI: interface-only, so no
  // data-ready interrupt is serviced).
  gpio_output_init(&dev->en_gpio, GPIO_OType_PP);
  gpio_output_set(&dev->en_gpio, true);
  gpio_input_init(&dev->int_input);

  uint8_t id = 0;
  if (!prv_read_whoami(dev, &id) ||
      (id != VC31_WHO_AM_I_VC31A && id != VC31_WHO_AM_I_VC31B)) {
    PBL_LOG_ERR("VC31 probe failed; WHO_AM_I 0x%02x", id);
    gpio_output_set(&dev->en_gpio, false);
    return;
  }

  dev->state->chip_id = id;
  dev->state->initialized = true;
  PBL_LOG_DBG("VC31%s found (id 0x%02x); interface-only (proprietary HR algo)",
              id == VC31_WHO_AM_I_VC31B ? "B" : "A", id);
}

bool hrm_enable(HRMDevice *dev) {
  if (!dev->state->initialized) {
    return false;
  }
  gpio_output_set(&dev->en_gpio, true);
  dev->state->enabled = true;
  return true;
}

void hrm_disable(HRMDevice *dev) {
  gpio_output_set(&dev->en_gpio, false);
  dev->state->enabled = false;
}

bool hrm_is_enabled(HRMDevice *dev) {
  return dev->state->enabled;
}
