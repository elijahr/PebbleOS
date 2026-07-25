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

// Known parts. GD25Q64 (C8 40 17) is industry-standard-confirmed. XT25F64B
// manufacturer byte 0x0B is Gordon's hardware-corrected constant (INFERRED vs
// JEP106; the capacity tier below protects us either way).
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
    return Bangle2FlashIdUnknown8Mb;
  }
  return Bangle2FlashIdUnknownReject;
}
