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
// Boot accepts a three-tier id classification instead of a single placeholder:
// known GD25Q64 / XT25F64B pass; a dead bus or wrong capacity byte fails loud;
// an unknown vendor with the right (8 MB) capacity byte warns and boots. See
// bangle2_flash_ids.h (shared with the active spi_nor/spi_nor.c backend) for
// the accepted set and the classifier itself, so the two backends cannot
// drift. This driver uses a generic dual-IO SPI NOR opcode set (JEDEC
// standard opcodes). No security-register / OTP support is exposed
// (num_sec_regs = 0), so otp_get_slot() degrades gracefully to "no serial"
// rather than requiring a modeled security-register array.
//
// Modeled on drivers/flash/gd25lq255e.c.

#include "board/board.h"
#include "drivers/flash/bangle2_flash_ids.h"
#include <pbl/drivers/flash/flash_impl.h>
#include <pbl/drivers/flash/qspi_flash.h>
#include <pbl/drivers/flash/qspi_flash_part_definitions.h>
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
// the classifier accepted the id (known part or unknown-8MB warn tier); see
// bangle2_flash_ids.h. Exposed as a module global (not just a local) so the
// emulator harness can read it back over the bus as direct evidence that the
// RDID was issued and classified, rather than inferring it from downstream
// boot progress.
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
    /* Part-struct default id (GigaDevice GD25Q64E, C8 40 17); NOT what gates
     * boot anymore. flash_impl_init() below now reads the raw id and runs it
     * through the shared bangle2_flash_ids.h classifier, so the accepted set
     * is the three-tier one, not a single-value compare against this field. */
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
  // resources, coredumps, or firmware storage. A watch fitted with a rejected
  // SPI-NOR part cannot boot correctly, so a dead bus or wrong capacity is
  // fatal rather than silently ignored -- this is the check that makes chip
  // detection real instead of vacuous. An unknown vendor with the right
  // capacity only warns: see bangle2_flash_ids.h for the full tier rationale.
  uint32_t jedec_id = 0;
  PBL_ASSERTN(qspi_flash_read_id(QSPI_FLASH, &jedec_id));
  const Bangle2FlashIdClass id_class = bangle2_flash_classify_jedec(jedec_id);
  s_flash_whoami_ok = (id_class == Bangle2FlashIdKnownGD25Q64) ||
                      (id_class == Bangle2FlashIdKnownXT25F64B) ||
                      (id_class == Bangle2FlashIdUnknown8Mb);
  switch (id_class) {
    case Bangle2FlashIdKnownGD25Q64:
      // Part string stays "GD25Q64E" (not "GD25Q64"): matches the existing
      // harness docstrings/greps and the pre-classifier INFO line -- no
      // needless divergence.
      PBL_LOG_INFO("QSPI flash %s detected (JEDEC 0x%06" PRIx32 ")", "GD25Q64E", jedec_id);
      break;
    case Bangle2FlashIdKnownXT25F64B:
      PBL_LOG_INFO("QSPI flash %s detected (JEDEC 0x%06" PRIx32 ")", "XT25F64B", jedec_id);
      break;
    case Bangle2FlashIdUnknown8Mb:
      // The %s arg is readable in loghashed captures, but 'U' (0x55) is the
      // NEWLOG frame-sync byte, so the harness greps the U-free fragment
      // "NKNOWN-8MB" -- the full token never appears verbatim in the stream.
      PBL_LOG_WRN("%s QSPI flash (JEDEC 0x%06" PRIx32
                  "): right capacity, unknown vendor; verify part and update "
                  "bangle2_flash_ids.h",
                  "UNKNOWN-8MB", jedec_id);
      break;
    case Bangle2FlashIdDeadBus:
    case Bangle2FlashIdUnknownReject:
      PBL_LOG_ERR("QSPI flash JEDEC id 0x%06" PRIx32
                  " %s -- refusing to boot (accepted: GD25Q64 0x001740C8, "
                  "XT25F64B 0x0017400B, or any 8 MB capacity byte 0x17)",
                  jedec_id, (id_class == Bangle2FlashIdDeadBus) ? "DEAD-BUS" : "REJECTED");
      PBL_ASSERT(s_flash_whoami_ok, "QSPI flash JEDEC id dead-bus/rejected");
      break;
  }

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
