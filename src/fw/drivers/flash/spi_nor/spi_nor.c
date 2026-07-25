/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// Generic SPI-NOR flash driver over the nRF52840 SPIM (EasyDMA) master.
//
// This is the hardware-faithful backend for the Bangle.js 2 (SMA-Q3) 8 MB
// external flash. The real watch wires a standard SPI NOR to plain GPIO pins
// (CS P0.14, SCK P0.16, MOSI/IO0 P0.15, MISO/IO1 P0.13) and Espruino bit-bangs
// the standard SPI-NOR command set over them -- it does NOT use the nRF52840
// QSPI peripheral. Driving those exact pins with the nRF52840 SPIM peripheral (a
// standard SPI master) plus a GPIO chip-select is therefore the faithful match:
// a standard SPI NOR spoken to by a standard SPI master. The prior QSPI-on-those
// -pins backend was an unverified emulator-vs-hardware fork; this replaces it.
//
// Each SPI-NOR command is one CS-framed full-duplex SPIM transfer. The opcode and
// 24-bit address (and, for page program, the data) are clocked out of TXD; for a
// read the same transfer keeps clocking (TX drained -> the ORC byte is sent) while
// the flash streams data back into RXD. CS is asserted (low) before the transfer
// and deasserted after, so the whole command is one continuous SPI transaction.
//
// The driver runs the SPIM in BLOCKING mode (nrfx handler == NULL): nrfx_spim_xfer
// busy-waits on EVENTS_END. This needs no FreeRTOS, so the same path serves both
// the normal and the core-dump (RTOS-less) callers of flash_impl_init().
//
// FIDELITY CAVEATS:
//  - The nRF52840 SPIM is a single-data-line master, so the real part's 0x3B
//    dual-output fast read is NOT used; the driver issues the universally supported
//    single-line 0x03 read. Correct for any SPI NOR, just not dual-IO speed.
//  - The concrete part / JEDEC id is UNKNOWN (Espruino never issues RDID). Boot
//    accepts a three-tier id classification instead of a single placeholder:
//    known GD25Q64 / XT25F64B pass; a dead bus or wrong capacity byte fails
//    loud; an unknown vendor with the right (8 MB) capacity byte warns and
//    boots. See bangle2_flash_ids.h (shared with the legacy bangle2_flash.c
//    backend) for the accepted set and the classifier itself.

#include "board/board.h"
#include "drivers/flash/bangle2_flash_ids.h"
#include "drivers/flash/flash_impl.h"
#include "drivers/gpio.h"
#include "flash_region/flash_region.h"
#include "kernel/util/delay.h"
#include "system/logging.h"
#include "system/passert.h"
#include "system/status_codes.h"
#include "pbl/util/math.h"
#include "pbl/util/size.h"

#include <hal/nrf_spim.h>
#include <nrfx_spim.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Standard JEDEC SPI-NOR opcodes.
#define SPI_NOR_OP_RDID 0x9FU        // read JEDEC id (3 bytes)
#define SPI_NOR_OP_READ 0x03U        // read data (single line)
#define SPI_NOR_OP_PP 0x02U          // page program (<=256 B)
#define SPI_NOR_OP_SE 0x20U          // 4 KB sector erase
#define SPI_NOR_OP_BE 0xD8U          // 64 KB block erase
#define SPI_NOR_OP_CE 0xC7U          // chip erase
#define SPI_NOR_OP_RDSR1 0x05U       // read status register 1 (bit0 = WIP)
#define SPI_NOR_OP_RDSR2 0x35U       // read status register 2 (bit7 = erase suspend)
#define SPI_NOR_OP_WREN 0x06U        // write enable
#define SPI_NOR_OP_WRDI 0x04U        // write disable
#define SPI_NOR_OP_ERASE_SUSPEND 0x75U
#define SPI_NOR_OP_ERASE_RESUME 0x7AU
#define SPI_NOR_OP_RSTEN 0x66U       // reset enable
#define SPI_NOR_OP_RST 0x99U         // reset
#define SPI_NOR_OP_DP 0xB9U          // deep power-down
#define SPI_NOR_OP_RDP 0xABU         // release deep power-down

// Status register 1 bit masks.
#define SPI_NOR_SR1_WIP (1U << 0U)
#define SPI_NOR_SR1_WEL (1U << 1U)
// Status register 2: erase-suspend flag.
#define SPI_NOR_SR2_ERASE_SUSPEND (1U << 7U)

// 3-byte / 24-bit addressing.
#define SPI_NOR_ADDR_LEN 4U  // opcode + 3 address bytes

// Largest data payload clocked in a single SPI-NOR command. A page program is
// capped at the 256-byte page anyway; reads are chunked to the same bound so one
// static RX bounce buffer covers both.
#define SPI_NOR_MAX_DATA 256U

// Reset recovery time. Renode ignores it; real parts settle in well under this.
#define SPI_NOR_RESET_LATENCY_US 12000U

// DMA buffers must live in RAM and be word-aligned for EasyDMA.
static uint8_t __attribute__((aligned(4))) s_tx_buf[SPI_NOR_ADDR_LEN + SPI_NOR_MAX_DATA];
static uint8_t __attribute__((aligned(4))) s_rx_buf[SPI_NOR_ADDR_LEN + SPI_NOR_MAX_DATA];

// Software write-protection window (this part exposes no hardware block lock).
static bool s_protected;
static FlashAddress s_protected_start;
static FlashAddress s_protected_end;

// Boot-time JEDEC chip-ID validation result. Set by flash_impl_init() once the
// classifier accepted the id (known part or unknown-8MB warn tier); see
// bangle2_flash_ids.h. Kept as a module global (matching the QSPI backend's
// symbol name) so the emulator harness can read it back as direct evidence the
// RDID was issued and classified.
static bool s_flash_whoami_ok;

// -----------------------------------------------------------------------------
// SPIM transaction helpers

static inline void prv_cs_assert(void) { gpio_output_set(&BOARD_CONFIG_FLASH.cs, true); }

static inline void prv_cs_deassert(void) { gpio_output_set(&BOARD_CONFIG_FLASH.cs, false); }

// One CS-framed full-duplex SPIM transfer. Clocks max(tx_len, rx_len) bytes: TX
// sends the opcode/address/data, and once TX is drained the ORC (0xFF) is clocked
// while RX (if any) captures the flash response.
static void prv_txn(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len) {
  nrfx_spim_xfer_desc_t desc = {
      .p_tx_buffer = tx,
      .tx_length = tx_len,
      .p_rx_buffer = rx,
      .rx_length = rx_len,
  };

  prv_cs_assert();
  nrfx_err_t err = nrfx_spim_xfer(&BOARD_CONFIG_FLASH.spi, &desc, 0);
  prv_cs_deassert();

  PBL_ASSERTN(err == NRFX_SUCCESS);
}

static void prv_cmd(uint8_t op) {
  uint8_t tx[1] = {op};
  prv_txn(tx, sizeof(tx), NULL, 0U);
}

static void prv_write_enable(void) { prv_cmd(SPI_NOR_OP_WREN); }

static uint8_t prv_read_sr1(void) {
  uint8_t tx[1] = {SPI_NOR_OP_RDSR1};
  uint8_t rx[2] = {0};

  prv_txn(tx, sizeof(tx), rx, sizeof(rx));
  return rx[1];
}

static uint8_t prv_read_sr2(void) {
  uint8_t tx[1] = {SPI_NOR_OP_RDSR2};
  uint8_t rx[2] = {0};

  prv_txn(tx, sizeof(tx), rx, sizeof(rx));
  return rx[1];
}

static bool prv_wip(void) { return (prv_read_sr1() & SPI_NOR_SR1_WIP) != 0U; }

static uint32_t prv_read_jedec_id(void) {
  uint8_t tx[1] = {SPI_NOR_OP_RDID};
  uint8_t rx[4] = {0};

  prv_txn(tx, sizeof(tx), rx, sizeof(rx));

  // rx[0] is clocked out during the opcode byte (don't-care); the 3 id bytes
  // follow. Pack LSB-first (manufacturer in bits[7:0]) per bangle2_flash_ids.h.
  return (uint32_t)rx[1] | ((uint32_t)rx[2] << 8U) | ((uint32_t)rx[3] << 16U);
}

static void prv_fill_addr(uint8_t *buf, uint8_t op, FlashAddress addr) {
  buf[0] = op;
  buf[1] = (uint8_t)((addr >> 16U) & 0xFFU);
  buf[2] = (uint8_t)((addr >> 8U) & 0xFFU);
  buf[3] = (uint8_t)(addr & 0xFFU);
}

static status_t prv_check_protected(FlashAddress addr) {
  if (!s_protected) {
    return S_SUCCESS;
  }

  if (addr < s_protected_start || addr > s_protected_end) {
    return S_SUCCESS;
  }

  return E_INVALID_OPERATION;
}

// -----------------------------------------------------------------------------
// flash_impl interface

status_t flash_impl_init(bool coredump_mode) {
  (void)coredump_mode;

  // CS is a plain GPIO the driver toggles per command (active low).
  gpio_output_init(&BOARD_CONFIG_FLASH.cs, GPIO_OType_PP);
  prv_cs_deassert();

  // SPIM in blocking mode (handler == NULL): the driver owns CS as a GPIO, so the
  // SPIM SS is left disconnected. MSB-first, mode 0 -- the SPI-NOR default.
  nrfx_spim_config_t config = NRFX_SPIM_DEFAULT_CONFIG(
      BOARD_CONFIG_FLASH.clk.gpio_pin, BOARD_CONFIG_FLASH.mosi.gpio_pin,
      BOARD_CONFIG_FLASH.miso.gpio_pin, NRF_SPIM_PIN_NOT_CONNECTED);
  config.frequency = BOARD_CONFIG_FLASH.clk_freq_hz;
  config.mode = NRF_SPIM_MODE_0;
  config.bit_order = NRF_SPIM_BIT_ORDER_MSB_FIRST;
  config.orc = 0xFFU;

  nrfx_err_t err = nrfx_spim_init(&BOARD_CONFIG_FLASH.spi, &config, NULL, NULL);
  PBL_ASSERTN(err == NRFX_SUCCESS);

  // Reset the part to abort any program/erase left in progress from before reboot.
  prv_cmd(SPI_NOR_OP_RSTEN);
  prv_cmd(SPI_NOR_OP_RST);
  delay_us(SPI_NOR_RESET_LATENCY_US);

  // Validate the attached flash by its JEDEC id before trusting it for PFS,
  // resources, coredumps, or firmware storage. A watch fitted with a rejected
  // SPI-NOR part cannot boot correctly, so a dead bus or wrong capacity is
  // fatal rather than silently ignored -- this is the check that makes chip
  // detection real instead of vacuous. An unknown vendor with the right
  // capacity only warns: see bangle2_flash_ids.h for the full tier rationale.
  const uint32_t jedec_id = prv_read_jedec_id();
  const Bangle2FlashIdClass id_class = bangle2_flash_classify_jedec(jedec_id);
  s_flash_whoami_ok = (id_class == Bangle2FlashIdKnownGD25Q64) ||
                      (id_class == Bangle2FlashIdKnownXT25F64B) ||
                      (id_class == Bangle2FlashIdUnknown8Mb);
  switch (id_class) {
    case Bangle2FlashIdKnownGD25Q64:
      // Part string stays "GD25Q64E" (not "GD25Q64"): matches the existing
      // harness docstrings/greps and the pre-classifier INFO line -- no
      // needless divergence.
      PBL_LOG_INFO("SPI-NOR flash %s detected (JEDEC 0x%06" PRIx32 ")", "GD25Q64E", jedec_id);
      break;
    case Bangle2FlashIdKnownXT25F64B:
      PBL_LOG_INFO("SPI-NOR flash %s detected (JEDEC 0x%06" PRIx32 ")", "XT25F64B", jedec_id);
      break;
    case Bangle2FlashIdUnknown8Mb:
      // The %s arg is a readable marker in loghashed captures (harness greps it).
      PBL_LOG_WRN("%s SPI-NOR (JEDEC 0x%06" PRIx32
                  "): right capacity, unknown vendor; verify part and update "
                  "bangle2_flash_ids.h",
                  "UNKNOWN-8MB", jedec_id);
      break;
    case Bangle2FlashIdDeadBus:
    case Bangle2FlashIdUnknownReject:
      PBL_LOG_ERR("SPI-NOR JEDEC id 0x%06" PRIx32
                  " %s -- refusing to boot (accepted: GD25Q64 0x001740C8, "
                  "XT25F64B 0x0017400B, or any 8 MB capacity byte 0x17)",
                  jedec_id, (id_class == Bangle2FlashIdDeadBus) ? "DEAD-BUS" : "REJECTED");
      PBL_ASSERT(s_flash_whoami_ok, "SPI-NOR flash JEDEC id dead-bus/rejected");
      break;
  }

  return S_SUCCESS;
}

status_t flash_impl_set_burst_mode(bool burst_mode) {
  // NYI
  return S_SUCCESS;
}

FlashAddress flash_impl_get_sector_base_address(FlashAddress addr) {
  return (addr & SECTOR_ADDR_MASK);
}

FlashAddress flash_impl_get_subsector_base_address(FlashAddress addr) {
  return (addr & SUBSECTOR_ADDR_MASK);
}

void flash_impl_enable_write_protection(void) {}

status_t flash_impl_write_protect(FlashAddress start_sector, FlashAddress end_sector) {
  if (s_protected) {
    return E_ERROR;
  }

  s_protected_start = start_sector;
  s_protected_end = end_sector;
  s_protected = true;

  return S_SUCCESS;
}

status_t flash_impl_unprotect(void) {
  if (!s_protected) {
    return E_ERROR;
  }

  s_protected = false;

  return S_SUCCESS;
}

status_t flash_impl_read_sync(void *buffer_ptr, FlashAddress start_addr, size_t buffer_size) {
  PBL_ASSERT(buffer_size > 0, "flash_impl_read_sync() called with 0 bytes to read");

  uint8_t *out = buffer_ptr;
  FlashAddress addr = start_addr;
  size_t remaining = buffer_size;

  while (remaining > 0U) {
    const size_t chunk = MIN(remaining, SPI_NOR_MAX_DATA);
    uint8_t tx[SPI_NOR_ADDR_LEN];

    prv_fill_addr(tx, SPI_NOR_OP_READ, addr);
    // Clock opcode+address, then keep clocking `chunk` more bytes so the flash
    // streams data into the RX bounce after the 4-byte command prefix.
    prv_txn(tx, sizeof(tx), s_rx_buf, SPI_NOR_ADDR_LEN + chunk);
    memcpy(out, &s_rx_buf[SPI_NOR_ADDR_LEN], chunk);

    out += chunk;
    addr += chunk;
    remaining -= chunk;
  }

  return S_SUCCESS;
}

int flash_impl_write_page_begin(const void *buffer, const FlashAddress start_addr, size_t len) {
  status_t status = prv_check_protected(start_addr);
  if (FAILED(status)) {
    return status;
  }

  // A page program cannot cross a 256-byte page boundary; clamp to the page and
  // to the single-transfer payload bound.
  len = MIN(len, PAGE_SIZE_BYTES - (start_addr % PAGE_SIZE_BYTES));
  len = MIN(len, SPI_NOR_MAX_DATA);

  prv_fill_addr(s_tx_buf, SPI_NOR_OP_PP, start_addr);
  memcpy(&s_tx_buf[SPI_NOR_ADDR_LEN], buffer, len);

  prv_write_enable();
  prv_txn(s_tx_buf, SPI_NOR_ADDR_LEN + len, NULL, 0U);

  return (int)len;
}

status_t flash_impl_get_write_status(void) { return prv_wip() ? E_BUSY : S_SUCCESS; }

static status_t prv_erase_begin(FlashAddress addr, uint8_t op) {
  // Address is a sector/subsector base from flash_api; keep the word-alignment
  // guard for parity with the QSPI backend.
  if ((addr & 0x3U) != 0U) {
    return E_INVALID_ARGUMENT;
  }

  uint8_t tx[SPI_NOR_ADDR_LEN];
  prv_fill_addr(tx, op, addr);

  prv_write_enable();
  prv_txn(tx, sizeof(tx), NULL, 0U);

  return S_SUCCESS;
}

status_t flash_impl_erase_subsector_begin(FlashAddress subsector_addr) {
  status_t status = prv_check_protected(subsector_addr);
  if (FAILED(status)) {
    return status;
  }
  return prv_erase_begin(subsector_addr, SPI_NOR_OP_SE);
}

status_t flash_impl_erase_sector_begin(FlashAddress sector_addr) {
  status_t status = prv_check_protected(sector_addr);
  if (FAILED(status)) {
    return status;
  }
  return prv_erase_begin(sector_addr, SPI_NOR_OP_BE);
}

status_t flash_impl_get_erase_status(void) {
  if (prv_wip()) {
    return E_BUSY;
  }

  // Not busy: distinguish "done" from "suspended" (erase-suspend flag in SR2).
  if ((prv_read_sr2() & SPI_NOR_SR2_ERASE_SUSPEND) != 0U) {
    return E_AGAIN;
  }

  return S_SUCCESS;
}

status_t flash_impl_erase_suspend(FlashAddress sector_addr) {
  if (!prv_wip()) {
    return S_NO_ACTION_REQUIRED;
  }

  prv_cmd(SPI_NOR_OP_ERASE_SUSPEND);
  return S_SUCCESS;
}

status_t flash_impl_erase_resume(FlashAddress sector_addr) {
  prv_cmd(SPI_NOR_OP_ERASE_RESUME);
  return S_SUCCESS;
}

status_t flash_impl_enter_low_power_mode(void) {
  prv_cmd(SPI_NOR_OP_DP);
  return S_SUCCESS;
}

status_t flash_impl_exit_low_power_mode(void) {
  prv_cmd(SPI_NOR_OP_RDP);
  return S_SUCCESS;
}

status_t flash_impl_blank_check_sector(FlashAddress addr) {
  const uint32_t size_bytes = SECTOR_SIZE_BYTES;
  uint32_t buffer[32];

  for (uint32_t offset = 0U; offset < size_bytes; offset += sizeof(buffer)) {
    flash_impl_read_sync(buffer, addr + offset, sizeof(buffer));
    for (uint32_t i = 0U; i < ARRAY_LENGTH(buffer); ++i) {
      if (buffer[i] != 0xFFFFFFFFUL) {
        return S_FALSE;
      }
    }
  }

  return S_TRUE;
}

status_t flash_impl_blank_check_subsector(FlashAddress addr) {
  const uint32_t size_bytes = SUBSECTOR_SIZE_BYTES;
  uint32_t buffer[32];

  for (uint32_t offset = 0U; offset < size_bytes; offset += sizeof(buffer)) {
    flash_impl_read_sync(buffer, addr + offset, sizeof(buffer));
    for (uint32_t i = 0U; i < ARRAY_LENGTH(buffer); ++i) {
      if (buffer[i] != 0xFFFFFFFFUL) {
        return S_FALSE;
      }
    }
  }

  return S_TRUE;
}

uint32_t flash_impl_get_typical_sector_erase_duration_ms(void) { return 150; }

uint32_t flash_impl_get_typical_subsector_erase_duration_ms(void) { return 50; }

// This part exposes no security-register / OTP array (num_sec_regs = 0), so
// otp_get_slot() degrades gracefully to "no serial". Any address is therefore
// invalid for the security-register ops.
status_t flash_impl_read_security_register(uint32_t addr, uint8_t *val) {
  return E_INVALID_ARGUMENT;
}

status_t flash_impl_security_register_is_locked(uint32_t address, bool *locked) {
  return E_INVALID_ARGUMENT;
}

status_t flash_impl_erase_security_register(uint32_t addr) { return E_INVALID_ARGUMENT; }

status_t flash_impl_write_security_register(uint32_t addr, uint8_t val) {
  return E_INVALID_ARGUMENT;
}

const FlashSecurityRegisters *flash_impl_security_registers_info(void) {
  static const FlashSecurityRegisters info = {
      .sec_regs = NULL,
      .num_sec_regs = 0,
      .sec_reg_size = 0,
  };
  return &info;
}

#ifdef CONFIG_RECOVERY_FW
status_t flash_impl_lock_security_register(uint32_t address) { return E_INVALID_ARGUMENT; }
#endif
