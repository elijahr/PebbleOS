/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Bootloader main entry. Walking-skeleton stub only: spins forever.
//! The real boot-selection logic (retained-page read, CRC check, image copy,
//! and jump to the firmware at 0x8000) lands in a following change.

void boot_main(void) {
  while (1) {
  }
}
