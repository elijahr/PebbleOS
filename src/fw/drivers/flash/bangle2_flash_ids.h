/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

// Bangle.js 2 SPI-NOR JEDEC id classification, shared by BOTH flash backends
// (spi_nor/spi_nor.c under CONFIG_FLASH_SPI_NOR -- the active bangle2 driver --
// and bangle2_flash.c under CONFIG_FLASH_BANGLE2, Kconfig-selectable) so the
// accepted-id set cannot drift between them.
//
// Packing convention (both drivers): the three RDID bytes are packed LSB-first:
// first RDID byte (manufacturer) in bits[7:0]; the capacity byte (third RDID
// byte) in bits[23:16].

#include <stdint.h>

// Known parts. GD25Q64 (C8 40 17) is industry-standard-confirmed, and the
// GigaDevice part is what Espruino documents for this board: EspruinoDocs
// info/Bangle.js2 Technical.md gives "8MB external flash (GD25Q64C/GD25Q64E)"
// with datasheet links (verified 2026-07-28). Both revisions share this JEDEC
// id, so the single constant covers them. NOTE that the Espruino board file
// boards/BANGLEJS2.py names NO manufacturer -- it defines only the pins, the
// 8 MB size, and memmap_base 0x60000000, which its own comment marks as a
// mapping done "in software". That address is an Espruino addressing
// convention, NOT an nRF52840 XIP window (the nRF52840 QSPI XIP region is
// 0x12000000, and this board does not use QSPI at all -- see spi_nor.c).
// XT25F64B manufacturer byte 0x0B is CONFIRMED against flashrom's
// flashchips.h (XTX_ID = 0x0B, XT25F64B model id 0x4017).
//
// *** THE BENCH UNIT IS THE XTX PART, NOT GIGADEVICE. ***
// An RDID (0x9F) read taken directly off this watch on 2026-07-30 returned
// 0B 40 17 -- manufacturer 0x0B (XTX), type 0x40, capacity 0x17 (8 MB) --
// which packs to BANGLE2_FLASH_JEDEC_XT25F64B (0x0017400B) and classifies as
// Bangle2FlashIdKnownXT25F64B. So despite the Espruino docs naming only the
// GigaDevice GD25Q64C/E, the silicon actually fitted to this unit is the XTX
// XT25F64B. The read was host-driven SPIM2 over SWD (no on-target code); see
// the R10 bootloader design doc section 12.1. Both parts share the standard
// SPI-NOR command set (0x9F/0x03/0x06/0x20/0xD8/0x02 + WIP polling), so the
// spi_nor driver is correct for either; the difference matters only for
// vendor-specific timing/erase-suspend details. GD25Q64 (C8 40 17) is kept as
// an accepted id too, since other units may carry it.
#define BANGLE2_FLASH_JEDEC_GD25Q64 0x001740C8UL
#define BANGLE2_FLASH_JEDEC_XT25F64B 0x0017400BUL
// Third RDID byte 0x17 = 2^23 bytes = 8 MB, the load-bearing capacity guard.
#define BANGLE2_FLASH_CAPACITY_8MB 0x17U

typedef enum Bangle2FlashIdClass {
  Bangle2FlashIdKnownGD25Q64,
  Bangle2FlashIdKnownXT25F64B,
  Bangle2FlashIdDeadBus,        // fail loud: nothing downstream can work
  Bangle2FlashIdUnknown8Mb,     // warn and boot: right capacity, unknown vendor
  Bangle2FlashIdUnknownReject,  // fail loud: wrong capacity corrupts the 8 MB map
} Bangle2FlashIdClass;

static inline Bangle2FlashIdClass bangle2_flash_classify_jedec(uint32_t id) {
  if (id == BANGLE2_FLASH_JEDEC_GD25Q64) {
    return Bangle2FlashIdKnownGD25Q64;
  }
  if (id == BANGLE2_FLASH_JEDEC_XT25F64B) {
    return Bangle2FlashIdKnownXT25F64B;
  }
  // 0x00000000 = MISO stuck low (SR1 would read 0x00); 0x00FFFFFF = MISO stuck
  // high (INFERRED mapping). Both mean a dead bus; both fail loud.
  if (id == 0x00000000UL || id == 0x00FFFFFFUL) {
    return Bangle2FlashIdDeadBus;
  }
  if (((id >> 16U) & 0xFFU) == BANGLE2_FLASH_CAPACITY_8MB) {
    // JEP106 has no manufacturer 0x00 or 0xFF: an 8 MB capacity byte paired
    // with either is partial bus garbage (e.g. 0x00170000), not a real part.
    // Reject it; the dead-bus tier above only covers all-0x00/all-0xFF ids.
    const uint8_t mfr = (uint8_t)(id & 0xFFU);
    if ((mfr == 0x00U) || (mfr == 0xFFU)) {
      return Bangle2FlashIdUnknownReject;
    }
    return Bangle2FlashIdUnknown8Mb;
  }
  return Bangle2FlashIdUnknownReject;
}
