/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// Flash region map for the Bangle.js 2 external SPI NOR flash.
//
// The Bangle.js 2 (SMA-Q3) carries an 8 MB (64 Mbit) dual-IO SPI NOR part on
// GPIO pins CS P0.14 / SCK P0.16 / IO0 P0.15 / IO1 P0.13. PebbleOS drives it
// through the nRF52840 QSPI peripheral (pins are selectable). This 8 MB layout
// mirrors the region set used by the 32 MB gd25lq255e map (asterix) but is
// scaled to fit the smaller part: the two 1 MB system-resource banks and the
// firmware slot are preserved so the resource store and OTA metadata resolve,
// while the filesystem soaks up the remaining space.

#define PAGE_SIZE_BYTES (0x100)

#define SECTOR_SIZE_BYTES (0x10000)
#define SECTOR_ADDR_MASK (~(SECTOR_SIZE_BYTES - 1))

#define SUBSECTOR_SIZE_BYTES (0x1000)
#define SUBSECTOR_ADDR_MASK (~(SUBSECTOR_SIZE_BYTES - 1))

// A bit of preprocessor magic to help with automatically calculating flash region addresses
//////////////////////////////////////////////////////////////////////////////

#define FLASH_REGION_DEF(MACRO, arg)                                                      \
  /* Protectable region (safe/recovery firmware image, lower 512K) */                     \
  MACRO(SAFE_FIRMWARE,           0x0080000 /*   512K */, arg) /* 0x0000000 - 0x007FFFF */ \
  MACRO(RSVD1,                   0x007F000 /*   508K */, arg) /* 0x0080000 - 0x00FEFFF */ \
  MACRO(MFG_INFO,                0x0001000 /*     4K */, arg) /* 0x00FF000 - 0x00FFFFF */ \
  /* Non-protectable region */                                                            \
  MACRO(FIRMWARE_SLOT_1,         0x0100000 /*  1024K */, arg) /* 0x0100000 - 0x01FFFFF */ \
  MACRO(SYSTEM_RESOURCES_BANK_0, 0x0100000 /*  1024K */, arg) /* 0x0200000 - 0x02FFFFF */ \
  MACRO(SYSTEM_RESOURCES_BANK_1, 0x0100000 /*  1024K */, arg) /* 0x0300000 - 0x03FFFFF */ \
  MACRO(FILESYSTEM,              0x03D0000 /*  3904K */, arg) /* 0x0400000 - 0x07CFFFF */ \
  MACRO(DEBUG_DB,                0x0020000 /*   128K */, arg) /* 0x07D0000 - 0x07EFFFF */ \
  MACRO(RSVD3,                   0x000D000 /*    52K */, arg) /* 0x07F0000 - 0x07FCFFF */ \
  MACRO(MFG_RESULTS,             0x0001000 /*     4K */, arg) /* 0x07FD000 - 0x07FDFFF */ \
  MACRO(MFG_BATTERY_STATE,       0x0001000 /*     4K */, arg) /* 0x07FE000 - 0x07FEFFF */ \
  MACRO(SHARED_PRF_STORAGE,      0x0001000 /*     4K */, arg) /* 0x07FF000 - 0x07FFFFF */

#include "flash_region_def_helper.h"

// Flash region _BEGIN and _END addresses
//////////////////////////////////////////////////////////////////////////////

#define FLASH_REGION_FIRMWARE_SLOT_1_BEGIN FLASH_REGION_START_ADDR(FIRMWARE_SLOT_1)
#define FLASH_REGION_FIRMWARE_SLOT_1_END FLASH_REGION_END_ADDR(FIRMWARE_SLOT_1)

#define FLASH_REGION_SYSTEM_RESOURCES_BANK_0_BEGIN FLASH_REGION_START_ADDR(SYSTEM_RESOURCES_BANK_0)
#define FLASH_REGION_SYSTEM_RESOURCES_BANK_0_END FLASH_REGION_END_ADDR(SYSTEM_RESOURCES_BANK_0)

#define FLASH_REGION_SYSTEM_RESOURCES_BANK_1_BEGIN FLASH_REGION_START_ADDR(SYSTEM_RESOURCES_BANK_1)
#define FLASH_REGION_SYSTEM_RESOURCES_BANK_1_END FLASH_REGION_END_ADDR(SYSTEM_RESOURCES_BANK_1)

#define FLASH_REGION_SAFE_FIRMWARE_BEGIN FLASH_REGION_START_ADDR(SAFE_FIRMWARE)
#define FLASH_REGION_SAFE_FIRMWARE_END FLASH_REGION_END_ADDR(SAFE_FIRMWARE)

#define FLASH_REGION_DEBUG_DB_BEGIN FLASH_REGION_START_ADDR(DEBUG_DB)
#define FLASH_REGION_DEBUG_DB_END FLASH_REGION_END_ADDR(DEBUG_DB)
#define FLASH_DEBUG_DB_BLOCK_SIZE SUBSECTOR_SIZE_BYTES

#define FLASH_REGION_FILESYSTEM_BEGIN FLASH_REGION_START_ADDR(FILESYSTEM)
#define FLASH_REGION_FILESYSTEM_END FLASH_REGION_END_ADDR(FILESYSTEM)
#define FLASH_FILESYSTEM_BLOCK_SIZE SUBSECTOR_SIZE_BYTES

#define FLASH_REGION_MFG_RESULTS_BEGIN FLASH_REGION_START_ADDR(MFG_RESULTS)
#define FLASH_REGION_MFG_RESULTS_END FLASH_REGION_END_ADDR(MFG_RESULTS)

#define FLASH_REGION_MFG_BATTERY_STATE_BEGIN FLASH_REGION_START_ADDR(MFG_BATTERY_STATE)
#define FLASH_REGION_MFG_BATTERY_STATE_END FLASH_REGION_END_ADDR(MFG_BATTERY_STATE)

#define FLASH_REGION_SHARED_PRF_STORAGE_BEGIN FLASH_REGION_START_ADDR(SHARED_PRF_STORAGE)
#define FLASH_REGION_SHARED_PRF_STORAGE_END FLASH_REGION_END_ADDR(SHARED_PRF_STORAGE)

#define FLASH_REGION_MFG_INFO_BEGIN FLASH_REGION_START_ADDR(MFG_INFO)
#define FLASH_REGION_MFG_INFO_END FLASH_REGION_END_ADDR(MFG_INFO)

#define BOARD_NOR_FLASH_SIZE FLASH_REGION_START_ADDR(_COUNT)

// Static asserts to make sure everything worked out
//////////////////////////////////////////////////////////////////////////////

// make sure all the sizes are multiples of the subsector size (4k)
FLASH_REGION_SIZE_CHECK(SUBSECTOR_SIZE_BYTES)

// make sure the total size is what we expect (8mb)
_Static_assert(BOARD_NOR_FLASH_SIZE == 0x800000, "Flash size should be 8mb");
