/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "drivers/flash/bangle2_flash_ids.h"

typedef struct IdCase {
  uint32_t id;
  Bangle2FlashIdClass expected;
} IdCase;

static const IdCase s_cases[] = {
    // Known parts -> pass.
    {0x001740C8UL, Bangle2FlashIdKnownGD25Q64},   // GD25Q64 (C8 40 17)
    {0x0017400BUL, Bangle2FlashIdKnownXT25F64B},  // XT25F64B (0B 40 17)
    // Dead bus -> fail-loud tier (MISO stuck low / stuck high).
    {0x00000000UL, Bangle2FlashIdDeadBus},
    {0x00FFFFFFUL, Bangle2FlashIdDeadBus},
    // Unknown vendor with the 8 MB capacity byte 0x17 -> warn-and-boot tier.
    {0x001740EFUL, Bangle2FlashIdUnknown8Mb},  // old Winbond placeholder id
    {0x001740AAUL, Bangle2FlashIdUnknown8Mb},  // arbitrary unknown vendor
    // Wrong capacity -> fail-loud reject tier.
    {0x001640C8UL, Bangle2FlashIdUnknownReject},  // 4 MB capacity byte 0x16
    {0x001840C8UL, Bangle2FlashIdUnknownReject},  // 16 MB capacity byte 0x18
};

void test_bangle2_flash_ids__classify_all_tiers(void) {
  for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); ++i) {
    cl_assert_equal_i((int)bangle2_flash_classify_jedec(s_cases[i].id), (int)s_cases[i].expected);
  }
}
