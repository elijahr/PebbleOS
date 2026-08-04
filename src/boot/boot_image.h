/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Pure image-validation decisions for the bangle2 bootloader.
//!
//! Split out of boot_main.c's image_validate() so the REJECT paths can be
//! unit-tested off-target. These functions touch no hardware: boot_main reads
//! the FirmwareDescription header from external NOR and streams the payload
//! through crc32(), then hands the resulting scalar fields here to decide
//! accept vs reject. Keeping the decisions here (rather than inline in the
//! hardware path) is what makes the corrupt-image / oversized-image rejects
//! testable without silicon.

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Legacy FirmwareDescription header length. An image whose description_length
//! field is not exactly this is not a format this bootloader understands.
#define FW_DESCRIPTION_LENGTH 12u

//! Structural (geometry) validation of a FirmwareDescription's length fields,
//! independent of hardware. `slot_capacity` is the number of bytes the internal
//! execution slot can hold (FW_EXEC_END - FW_EXEC_BASE in boot_main). Rejects:
//!   - description_length != FW_DESCRIPTION_LENGTH (unknown header format)
//!   - firmware_length == 0 (nothing to boot)
//!   - firmware_length > slot_capacity (the brick guard: refuse BEFORE erase an
//!     image whose copy would overrun the execution slot)
//! A payload exactly the size of the slot is accepted (comparison is `>`).
static inline bool boot_image_geometry_valid(uint32_t description_length,
                                             uint32_t firmware_length,
                                             uint32_t slot_capacity) {
  if (description_length != FW_DESCRIPTION_LENGTH) {
    return false;
  }
  if (firmware_length == 0 || firmware_length > slot_capacity) {
    return false;
  }
  return true;
}

//! Whether a computed payload CRC-32 matches the descriptor's stored checksum.
//! The caller computes `computed_crc` over the payload bytes with crc32(); this
//! is the corrupt-image accept/reject gate expressed as a pure decision so both
//! the source-image check and the post-copy internal check share (and test) it.
static inline bool boot_image_checksum_valid(uint32_t computed_crc,
                                             uint32_t expected_checksum) {
  return computed_crc == expected_checksum;
}
