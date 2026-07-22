/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// Flash part driver for the Bangle.js 2 (SMA-Q3) external SPI NOR.
//
// The concrete part number is not named in the Espruino source: the Bangle.js 2
// (BANGLEJS_Q3, boards/BANGLEJS2.py) bit-bangs plain GPIO SPI to its 8 MB flash
// (CS D14 / SCK D16 / MOSI/IO0 D15 / MISO/IO1 D13), NOT the nRF52840 QSPI
// peripheral, and never issues RDID — so only the capacity (8 MB / 64 Mbit), the
// pins, and the dual-IO 2x-read capability are known from the vendor source, and
// the true JEDEC id is UNKNOWN pending a hardware teardown.
//
// UNRESOLVED FORK: this driver targets the nRF52840 QSPI peripheral (via
// qspi_flash_init / nrf5 qspi.c), which is an emulator-vs-hardware divergence —
// the real watch would need either the QSPI peripheral routed to those GPIOs or
// a bit-banged/SPIM SPI-NOR driver. Flagged for a design decision.
//
// Because the emulator must be able to *fail* a wrong flash driver at boot
// (making chip-ID validation real instead of vacuous), a concrete real part is
// ASSUMED here as a defensible placeholder: GigaDevice GD25Q64E, a
// current-production 64 Mbit dual-IO SPI-NOR whose RDID (0x9F) returns C8 40 17.
// This is NOT silicon-confirmed for the SMA-Q3 — if a teardown names a different
// part, update qspi_id_value / name to match. The boot whoami check below proves
// the validation MECHANISM works (RDID issued, compared, mismatch fails boot); it
// does NOT prove the silicon identity, since the model is co-tuned to this same
// assumed id. This driver uses a generic dual-IO SPI NOR opcode set (JEDEC
// standard opcodes). No security-register / OTP support is exposed
// (num_sec_regs = 0), so otp_get_slot() degrades gracefully to "no serial"
// rather than requiring a modeled security-register array.
//
// Modeled on drivers/flash/gd25lq255e.c.

#include "board/board.h"
#include "drivers/flash/flash_impl.h"
#include "drivers/flash/qspi_flash.h"
#include "drivers/flash/qspi_flash_part_definitions.h"
#include "flash_region/flash_region.h"
#include "system/logging.h"
#include "system/passert.h"
#include "system/status_codes.h"
#include "pbl/util/math.h"
#include "pbl/util/size.h"

#include <inttypes.h>

static bool s_protected;
static FlashAddress s_protected_start;
static FlashAddress s_protected_end;

// Boot-time JEDEC chip-ID validation result. Set by flash_impl_init(): true once
// qspi_flash_check_whoami() has confirmed the attached part matches
// QSPI_FLASH_PART.qspi_id_value. Exposed as a module global (not just a local)
// so the emulator harness can read it back over the bus as direct evidence that
// the RDID was issued and matched, rather than inferring it from downstream boot
// progress.
static bool s_flash_whoami_ok;

static QSPIFlashPart QSPI_FLASH_PART = {
    .instructions =
        {
            .fast_read = 0x0B,
            .read2o = 0x3B,
            .read2io = 0xBB,
            .read4o = 0x6B,
            .read4io = 0xEB,
            .pp = 0x02,
            .pp4o = 0x32,
            .erase_sector_4k = 0x20,
            .erase_block_64k = 0xD8,
            .write_enable = 0x06,
            .write_disable = 0x04,
            .rdsr1 = 0x05,
            .rdsr2 = 0x35,
            .wrsr = 0x01,
            .erase_suspend = 0x75,
            .erase_resume = 0x7A,
            .enter_low_power = 0xB9,
            .exit_low_power = 0xAB,
            .enter_quad_mode = 0x38,
            .reset_enable = 0x66,
            .reset = 0x99,
            .qspi_id = 0x9F,
            .en4b = 0xB7,
        },
    .status_bit_masks =
        {
            .busy = 1 << 0,
            .write_enable = 1 << 1,
        },
    .flag_status_bit_masks =
        {
            .erase_suspend = 1 << 7,
        },
    .dummy_cycles =
        {
            .fast_read = 8,
        },
    .sec_registers =
        {
            .sec_regs = NULL,
            .num_sec_regs = 0,
            .sec_reg_size = 0,
        },
    .supports_block_lock = false,
    .reset_latency_ms = 12,
    .suspend_to_read_latency_us = 20,
    .standby_to_low_power_latency_us = 3,
    .low_power_to_standby_latency_us = 20,
    .supports_fast_read_ddr = false,
    /* Dual-IO reads need no quad-enable bit. */
    .qer_type = JESD216_DW15_QER_NONE,
    /* ASSUMED PLACEHOLDER (see header): GigaDevice GD25Q64E RDID (0x9F) C8 40 17 ->
     * manufacturer 0xC8 (GigaDevice), device 0x4017 (64 Mbit). Enforced at boot by
     * flash_impl_init() below. Real Bangle.js 2 JEDEC id is UNVERIFIED. */
    .qspi_id_value = 0x001740c8,
    .size = 0x800000, /* 8 MB */
    .name = "GD25Q64E",
};

static status_t prv_flash_check_protected(FlashAddress addr) {
  if (!s_protected) {
    return S_SUCCESS;
  }

  if (addr < s_protected_start || addr > s_protected_end) {
    return S_SUCCESS;
  }

  return E_INVALID_OPERATION;
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

status_t flash_impl_init(bool coredump_mode) {
  qspi_flash_init(QSPI_FLASH, &QSPI_FLASH_PART, coredump_mode);

  // Validate the attached flash by its JEDEC id before trusting it for PFS,
  // resources, coredumps, or firmware storage. A watch fitted with the wrong
  // SPI-NOR part cannot boot correctly, so a mismatch is fatal rather than
  // silently ignored -- this is the check that makes chip detection real instead
  // of vacuous. (nrf5 qspi_flash_check_whoami() reads 3 RDID bytes into a
  // uint32_t; on this port Renode zero-fills RAM so the untouched MSB reads 0 and
  // the compare is exact. See the reconciliation note in the harness / report:
  // on real silicon that MSB is uninitialized stack and the shared nrf5 driver
  // should zero-init it.)
  s_flash_whoami_ok = qspi_flash_check_whoami(QSPI_FLASH);
  if (!s_flash_whoami_ok) {
    PBL_LOG_ERR("QSPI flash WHOAMI mismatch: attached part is not %s (expected JEDEC 0x%06" PRIx32
                ") -- refusing to boot on unknown flash",
                QSPI_FLASH_PART.name, QSPI_FLASH_PART.qspi_id_value);
    PBL_ASSERT(s_flash_whoami_ok, "QSPI flash JEDEC id mismatch");
  }
  PBL_LOG_INFO("QSPI flash %s detected (JEDEC 0x%06" PRIx32 ")", QSPI_FLASH_PART.name,
               QSPI_FLASH_PART.qspi_id_value);

  return S_SUCCESS;
}

status_t flash_impl_get_erase_status(void) { return qspi_flash_is_erase_complete(QSPI_FLASH); }

status_t flash_impl_erase_subsector_begin(FlashAddress subsector_addr) {
  status_t status;

  status = prv_flash_check_protected(subsector_addr);
  if (FAILED(status)) {
    return status;
  }

  return qspi_flash_erase_begin(QSPI_FLASH, subsector_addr, true /* is_subsector */);
}
status_t flash_impl_erase_sector_begin(FlashAddress sector_addr) {
  status_t status;

  status = prv_flash_check_protected(sector_addr);
  if (FAILED(status)) {
    return status;
  }

  return qspi_flash_erase_begin(QSPI_FLASH, sector_addr, false /* !is_subsector */);
}

status_t flash_impl_erase_suspend(FlashAddress sector_addr) {
  return qspi_flash_erase_suspend(QSPI_FLASH, sector_addr);
}

status_t flash_impl_erase_resume(FlashAddress sector_addr) {
  qspi_flash_erase_resume(QSPI_FLASH, sector_addr);
  return S_SUCCESS;
}

status_t flash_impl_read_sync(void *buffer_ptr, FlashAddress start_addr, size_t buffer_size) {
  PBL_ASSERT(buffer_size > 0, "flash_impl_read_sync() called with 0 bytes to read");
  qspi_flash_read_blocking(QSPI_FLASH, start_addr, buffer_ptr, buffer_size);
  return S_SUCCESS;
}

int flash_impl_write_page_begin(const void *buffer, const FlashAddress start_addr, size_t len) {
  status_t status;

  status = prv_flash_check_protected(start_addr);
  if (FAILED(status)) {
    return status;
  }

  return qspi_flash_write_page_begin(QSPI_FLASH, buffer, start_addr, len);
}

status_t flash_impl_get_write_status(void) { return qspi_flash_get_write_status(QSPI_FLASH); }

status_t flash_impl_enter_low_power_mode(void) {
  qspi_flash_set_lower_power_mode(QSPI_FLASH, true);
  return S_SUCCESS;
}
status_t flash_impl_exit_low_power_mode(void) {
  qspi_flash_set_lower_power_mode(QSPI_FLASH, false);
  return S_SUCCESS;
}

status_t flash_impl_set_burst_mode(bool burst_mode) {
  // NYI
  return S_SUCCESS;
}

status_t flash_impl_blank_check_sector(FlashAddress addr) {
  return qspi_flash_blank_check(QSPI_FLASH, addr, false /* !is_subsector */);
}
status_t flash_impl_blank_check_subsector(FlashAddress addr) {
  return qspi_flash_blank_check(QSPI_FLASH, addr, true /* is_subsector */);
}

uint32_t flash_impl_get_typical_sector_erase_duration_ms(void) { return 150; }

uint32_t flash_impl_get_typical_subsector_erase_duration_ms(void) { return 50; }

status_t flash_impl_read_security_register(uint32_t addr, uint8_t *val) {
  return qspi_flash_read_security_register(QSPI_FLASH, addr, val);
}

status_t flash_impl_security_register_is_locked(uint32_t address, bool *locked) {
  return qspi_flash_security_register_is_locked(QSPI_FLASH, address, locked);
}

status_t flash_impl_erase_security_register(uint32_t addr) {
  return qspi_flash_erase_security_register(QSPI_FLASH, addr);
}

status_t flash_impl_write_security_register(uint32_t addr, uint8_t val) {
  return qspi_flash_write_security_register(QSPI_FLASH, addr, val);
}

const FlashSecurityRegisters *flash_impl_security_registers_info(void) {
  return qspi_flash_security_registers_info(QSPI_FLASH);
}

#ifdef CONFIG_RECOVERY_FW
status_t flash_impl_lock_security_register(uint32_t address) {
  return qspi_flash_lock_security_register(QSPI_FLASH, address);
}
#endif
