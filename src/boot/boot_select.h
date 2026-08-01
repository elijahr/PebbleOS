/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Pure boot-image-selection state machine for the bangle2 bootloader.
//!
//! Split out of boot_main.c so it can be unit-tested off-target: it touches no
//! hardware, only the boot-bit word and the retained-page-valid flag. boot_main
//! wraps it with the actual NOR read / flash copy / jump.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// Boot bits (mirror of src/fw/system/bootbits.h + rtc_registers.h)

#define BOOT_BIT_NEW_FW_AVAILABLE (1u << 1)
#define BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS (1u << 2)
#define BOOT_BIT_FW_START_FAIL_STRIKE_ONE (1u << 3)
#define BOOT_BIT_FW_START_FAIL_STRIKE_TWO (1u << 4)
#define BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE (1u << 5)
#define BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO (1u << 6)
#define BOOT_BIT_NEW_FW_INSTALLED (1u << 15)
#define BOOT_BIT_FORCE_PRF (1u << 17)

// -----------------------------------------------------------------------------
// External NOR sources (flash_region_bangle2.h)

#define NOR_SAFE_FIRMWARE 0x00000000u    // PRF image (512 KiB region)
#define NOR_FIRMWARE_SLOT_1 0x00100000u  // OTA / PutBytes staging (1024 KiB)

// -----------------------------------------------------------------------------

//! What the bootloader should do this boot, decided purely from the boot bits.
typedef struct {
  bool want_copy;        //!< false: start the resident image at 0x8000 as-is
  uint32_t src_addr;     //!< NOR address to copy from (when want_copy)
  uint32_t consume_bit;  //!< the request bit to clear once handled
  uint32_t strike_one;   //!< first-failure strike bit for this source
  uint32_t strike_two;   //!< second-failure strike bit for this source
  uint32_t install_bit;  //!< bit to set on a successful normal-fw install (else 0)
} BootPlan;

//! Decide the boot plan. FORCE_PRF wins over NEW_FW_AVAILABLE (an explicit
//! recovery request beats a staged update). An untrusted retained page (CRC
//! failed) yields no copy: start the resident firmware.
static inline BootPlan boot_select(uint32_t boot_bits, bool retained_valid) {
  BootPlan plan = {0};
  if (!retained_valid) {
    return plan;
  }
  if (boot_bits & BOOT_BIT_FORCE_PRF) {
    plan.want_copy = true;
    plan.src_addr = NOR_SAFE_FIRMWARE;
    plan.consume_bit = BOOT_BIT_FORCE_PRF;
    plan.strike_one = BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE;
    plan.strike_two = BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO;
    plan.install_bit = 0;  // PRF install is not tracked with NEW_FW_INSTALLED
  } else if (boot_bits & BOOT_BIT_NEW_FW_AVAILABLE) {
    plan.want_copy = true;
    plan.src_addr = NOR_FIRMWARE_SLOT_1;
    plan.consume_bit = BOOT_BIT_NEW_FW_AVAILABLE;
    plan.strike_one = BOOT_BIT_FW_START_FAIL_STRIKE_ONE;
    plan.strike_two = BOOT_BIT_FW_START_FAIL_STRIKE_TWO;
    plan.install_bit = BOOT_BIT_NEW_FW_INSTALLED;
  }
  return plan;
}

//! Which strike bit to set given a plan and the bits already present: escalate
//! to strike_two only if strike_one is already set.
static inline uint32_t boot_next_strike(const BootPlan *plan, uint32_t boot_bits) {
  return (boot_bits & plan->strike_one) ? plan->strike_two : plan->strike_one;
}
