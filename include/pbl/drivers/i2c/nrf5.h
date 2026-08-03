/* SPDX-FileCopyrightText: 2025 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include <nrfx_twim.h>
#pragma GCC diagnostic pop

//! Selects which HAL implementation backs an I2C bus. All four nRF52840
//! hardware serial instances are consumed by the display (SPIM3), flash
//! (SPIM2), touch (TWIM0) and accel (TWIM1) buses, so sensors that live on
//! spare GPIOs (HRM / mag / pressure) must be driven by a software bit-bang
//! bus. A board declares each bus with one of these types; the common nRF HAL
//! (nrf5.c) dispatches every i2c_hal_* entry point on it.
typedef enum I2CBusHalType {
  //! Hardware nrfx TWIM (EasyDMA) controller. Uses .twim / .frequency.
  I2CBusHalType_Twim = 0,
  //! Software bit-bang master over two GPIOs. Uses the bus SCL/SDA pins in
  //! I2CBus (definitions.h) plus .bitbang_half_period_us below.
  I2CBusHalType_BitBang,
} I2CBusHalType;

typedef struct I2CBusHal {
  //! Backing implementation for this bus. Defaults to Twim (0) so existing
  //! board declarations that omit it keep the hardware path.
  I2CBusHalType type;

  //! nrfx TWIM instance (valid when type == I2CBusHalType_Twim).
  nrfx_twim_t twim;
  //! Bus clock speed (valid when type == I2CBusHalType_Twim).
  nrf_twim_frequency_t frequency;

  //! Half clock period for the bit-bang master, in microseconds (valid when
  //! type == I2CBusHalType_BitBang). Loose in emulation; a small value keeps a
  //! ~100-400 kHz-equivalent clock on real hardware.
  uint32_t bitbang_half_period_us;
} I2CBusHal;

//! Bit-bang HAL entry points (drivers/i2c/bitbang.c). Called by the nRF HAL
//! dispatcher (nrf5.c) when a bus is declared I2CBusHalType_BitBang.
void i2c_bitbang_hal_init(I2CBus *bus);
void i2c_bitbang_hal_enable(I2CBus *bus);
void i2c_bitbang_hal_disable(I2CBus *bus);
bool i2c_bitbang_hal_is_busy(I2CBus *bus);
void i2c_bitbang_hal_abort_transfer(I2CBus *bus);
void i2c_bitbang_hal_start_transfer(I2CBus *bus);
