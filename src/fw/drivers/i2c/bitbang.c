/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Software (bit-bang) I2C master HAL for the nRF52840.
//!
//! All four nRF52840 hardware serial instances are already consumed on the
//! Bangle.js 2 (display SPIM3, flash SPIM2, touch TWIM0, accel TWIM1), so the
//! sensors that live on spare GPIOs (HRM / mag / pressure) must be driven by a
//! software I2C master, exactly as Espruino bit-bangs them. This file backs a
//! bus declared I2CBusHalType_BitBang; the common nRF HAL (nrf5.c) dispatches
//! every i2c_hal_* entry point here so the sensor drivers keep using the same
//! i2c_use / i2c_read_register_block / i2c_write_block API as a hardware bus.
//!
//! Line discipline (single-master, no clock stretching):
//!   * SCL is driven push-pull (the master owns the clock; stretching is not
//!     modeled). High = release-to-idle level, low = clock the bus.
//!   * SDA is open-drain from the master's side: the master drives it low or
//!     high while transmitting (address / register / write data), and
//!     tri-states it (input) while receiving (ACK check, read data) so the
//!     slave can drive it. An external pull-up holds the released line high; an
//!     internal pull-up is enabled as a fallback.
//!
//! The transfer runs synchronously to completion inside i2c_hal_start_transfer
//! and reports the result through the common i2c_handle_transfer_event path, so
//! the common.c wait-on-semaphore loop behaves the same as for the IRQ-driven
//! TWIM HAL.

#include "drivers/i2c.h"
#include "definitions.h"
#include "nrf5.h"

#include "system/logging.h"

#include <hal/nrf_gpio.h>

PBL_LOG_MODULE_DECLARE(driver_i2c, CONFIG_DRIVER_I2C_LOG_LEVEL);

#define I2C_READ_WRITE_BIT (0x01)

//! Loose busy-wait. In Renode this advances virtual time negligibly; on real
//! hardware it sets the bus clock rate (~100-400 kHz at a few microseconds per
//! half period). Volatile so the loop is not optimized away.
static void prv_delay_us(uint32_t us) {
  volatile uint32_t loops = (us * 4u) + 1u;
  while (loops--) {
  }
}

static void prv_half_period(I2CBus *bus) {
  prv_delay_us(bus->hal->bitbang_half_period_us);
}

static void prv_scl_set(I2CBus *bus, bool high) {
  nrf_gpio_pin_write(bus->scl_gpio.gpio_pin, high);
}

//! Actively drive SDA (master transmitting). Push-pull is used for the driven
//! levels so both edges are deterministic; the line only floats when released.
static void prv_sda_drive(I2CBus *bus, bool high) {
  nrf_gpio_cfg_output(bus->sda_gpio.gpio_pin);
  nrf_gpio_pin_write(bus->sda_gpio.gpio_pin, high);
}

//! Tri-state SDA so the slave can drive it (master receiving / ACK check).
static void prv_sda_release(I2CBus *bus) {
  nrf_gpio_cfg_input(bus->sda_gpio.gpio_pin, NRF_GPIO_PIN_PULLUP);
}

static bool prv_sda_read(I2CBus *bus) {
  return nrf_gpio_pin_read(bus->sda_gpio.gpio_pin) != 0U;
}

static void prv_start(I2CBus *bus) {
  // START = SDA falling while SCL is high.
  prv_sda_drive(bus, true);
  prv_scl_set(bus, true);
  prv_half_period(bus);
  prv_sda_drive(bus, false);
  prv_half_period(bus);
  prv_scl_set(bus, false);
  prv_half_period(bus);
}

static void prv_stop(I2CBus *bus) {
  // STOP = SDA rising while SCL is high. The rising edge is driven push-pull so
  // it is a deterministic GPIO edge, then the line is released to idle.
  prv_sda_drive(bus, false);
  prv_half_period(bus);
  prv_scl_set(bus, true);
  prv_half_period(bus);
  prv_sda_drive(bus, true);
  prv_half_period(bus);
  prv_sda_release(bus);
  prv_half_period(bus);
}

static void prv_write_bit(I2CBus *bus, bool bit) {
  prv_sda_drive(bus, bit);
  prv_half_period(bus);
  prv_scl_set(bus, true);
  prv_half_period(bus);
  prv_scl_set(bus, false);
}

//! Clock in one bit from the slave. SDA must already be released by the caller.
static bool prv_read_bit(I2CBus *bus) {
  prv_half_period(bus);
  prv_scl_set(bus, true);
  prv_half_period(bus);
  bool bit = prv_sda_read(bus);
  prv_scl_set(bus, false);
  return bit;
}

//! Write a byte MSB-first and return true if the slave ACKed (SDA low on the
//! 9th clock).
static bool prv_write_byte(I2CBus *bus, uint8_t byte) {
  for (int i = 7; i >= 0; i--) {
    prv_write_bit(bus, (byte >> i) & 1U);
  }
  // ACK slot: release SDA, clock once, sample. ACK == low.
  prv_sda_release(bus);
  prv_half_period(bus);
  prv_scl_set(bus, true);
  prv_half_period(bus);
  bool ack = !prv_sda_read(bus);
  prv_scl_set(bus, false);
  prv_half_period(bus);
  return ack;
}

//! Read a byte MSB-first, then drive ACK (low) if \a ack, else NACK (high).
static uint8_t prv_read_byte(I2CBus *bus, bool ack) {
  uint8_t byte = 0;
  prv_sda_release(bus);
  for (int i = 0; i < 8; i++) {
    byte = (uint8_t)((byte << 1) | (prv_read_bit(bus) ? 1U : 0U));
  }
  // Master drives the ACK/NACK bit: ACK = low, NACK = high.
  prv_write_bit(bus, !ack);
  return byte;
}

void i2c_bitbang_hal_init(I2CBus *bus) {
  // Idle the bus: SCL high (output), SDA released high.
  nrf_gpio_pin_write(bus->scl_gpio.gpio_pin, true);
  nrf_gpio_cfg_output(bus->scl_gpio.gpio_pin);
  nrf_gpio_pin_write(bus->scl_gpio.gpio_pin, true);
  prv_sda_release(bus);
}

void i2c_bitbang_hal_enable(I2CBus *bus) {
  nrf_gpio_cfg_output(bus->scl_gpio.gpio_pin);
  nrf_gpio_pin_write(bus->scl_gpio.gpio_pin, true);
  prv_sda_release(bus);
}

void i2c_bitbang_hal_disable(I2CBus *bus) {
  // Park both lines as inputs so the bus is high-impedance when unused.
  nrf_gpio_cfg_input(bus->scl_gpio.gpio_pin, NRF_GPIO_PIN_PULLUP);
  nrf_gpio_cfg_input(bus->sda_gpio.gpio_pin, NRF_GPIO_PIN_PULLUP);
}

bool i2c_bitbang_hal_is_busy(I2CBus *bus) {
  // Transfers run synchronously to completion; the bus is never left busy.
  return false;
}

void i2c_bitbang_hal_abort_transfer(I2CBus *bus) {
  // Nothing to abort for a synchronous transfer; re-idle the bus.
  i2c_bitbang_hal_enable(bus);
}

void i2c_bitbang_hal_start_transfer(I2CBus *bus) {
  I2CTransfer *transfer = &bus->state->transfer;
  const uint8_t addr7 = (uint8_t)((transfer->device_address >> 1) & 0x7FU);
  const uint8_t addr_wr = (uint8_t)(addr7 << 1);
  const uint8_t addr_rd = (uint8_t)((addr7 << 1) | I2C_READ_WRITE_BIT);
  bool ok = true;

  if (transfer->type == I2CTransferType_SendRegisterAddress) {
    prv_start(bus);
    ok = prv_write_byte(bus, addr_wr);
    if (ok) {
      ok = prv_write_byte(bus, transfer->register_address);
    }
    if (ok && transfer->direction == I2CTransferDirection_Read) {
      // Repeated START, then read.
      prv_start(bus);
      ok = prv_write_byte(bus, addr_rd);
      for (uint32_t i = 0; ok && i < transfer->size; i++) {
        bool last = (i + 1U == transfer->size);
        transfer->data[i] = prv_read_byte(bus, !last);
      }
    } else if (ok) {
      for (uint32_t i = 0; ok && i < transfer->size; i++) {
        ok = prv_write_byte(bus, transfer->data[i]);
      }
    }
  } else {  // I2CTransferType_NoRegisterAddress
    prv_start(bus);
    if (transfer->direction == I2CTransferDirection_Read) {
      ok = prv_write_byte(bus, addr_rd);
      for (uint32_t i = 0; ok && i < transfer->size; i++) {
        bool last = (i + 1U == transfer->size);
        transfer->data[i] = prv_read_byte(bus, !last);
      }
    } else {
      ok = prv_write_byte(bus, addr_wr);
      for (uint32_t i = 0; ok && i < transfer->size; i++) {
        ok = prv_write_byte(bus, transfer->data[i]);
      }
    }
  }

  prv_stop(bus);

  if (!ok) {
    PBL_LOG_ERR("bit-bang I2C NACK on bus %s (addr 0x%02x)", bus->name, addr7);
  }

  // Report through the common completion path. On a bit-bang bus a NACK is a
  // hard error (no MFi busy-NACK retry semantics), so it maps to Error, which
  // common.c treats as a terminal failed transfer.
  i2c_handle_transfer_event(bus, ok ? I2CTransferEvent_TransferComplete
                                    : I2CTransferEvent_Error);
}
