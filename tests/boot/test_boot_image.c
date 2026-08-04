/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "boot_image.h"
#include "pbl/util/crc32.h"

// boot_image_geometry_valid() and boot_image_checksum_valid() are pure; no
// fixtures needed. crc32() is linked from libutil by the clar harness.
void test_boot_image__initialize(void) {}
void test_boot_image__cleanup(void) {}

// A small, arbitrary execution-slot capacity used to exercise the brick guard
// boundary. The pure geometry function takes capacity as a parameter, so the
// test is independent of the real FW_EXEC_END - FW_EXEC_BASE layout value.
#define TEST_SLOT_CAPACITY 0x1000u  // 4096 bytes

// -----------------------------------------------------------------------------
// Geometry gate rejects

// description_length must be exactly 12 (the legacy FirmwareDescription size).
// A too-short header is rejected.
void test_boot_image__desc_len_too_small_rejects(void) {
  cl_assert(!boot_image_geometry_valid(FW_DESCRIPTION_LENGTH - 1, 256, TEST_SLOT_CAPACITY));
}

// A too-long description_length is rejected too (guards against a header format
// this bootloader does not understand).
void test_boot_image__desc_len_too_large_rejects(void) {
  cl_assert(!boot_image_geometry_valid(FW_DESCRIPTION_LENGTH + 1, 256, TEST_SLOT_CAPACITY));
}

// An empty payload (firmware_length == 0) is rejected: there is nothing to copy
// or boot.
void test_boot_image__zero_firmware_length_rejects(void) {
  cl_assert(!boot_image_geometry_valid(FW_DESCRIPTION_LENGTH, 0, TEST_SLOT_CAPACITY));
}

// The brick guard: a payload larger than the execution slot is rejected BEFORE
// any erase. Without this, image_copy would erase/write past the slot end.
void test_boot_image__firmware_length_over_capacity_rejects(void) {
  cl_assert(!boot_image_geometry_valid(FW_DESCRIPTION_LENGTH, TEST_SLOT_CAPACITY + 1,
                                       TEST_SLOT_CAPACITY));
}

// -----------------------------------------------------------------------------
// Geometry gate boundary + accepts

// The comparison is `firmware_length > slot_capacity`, so a payload EXACTLY the
// size of the slot is accepted. A `>=` mutation would wrongly reject here.
void test_boot_image__firmware_length_equals_capacity_accepts(void) {
  cl_assert(boot_image_geometry_valid(FW_DESCRIPTION_LENGTH, TEST_SLOT_CAPACITY,
                                      TEST_SLOT_CAPACITY));
}

// Just under capacity is accepted.
void test_boot_image__firmware_length_just_under_capacity_accepts(void) {
  cl_assert(boot_image_geometry_valid(FW_DESCRIPTION_LENGTH, TEST_SLOT_CAPACITY - 1,
                                      TEST_SLOT_CAPACITY));
}

// The smallest non-empty payload (1 byte) with a well-formed header is accepted
// (positive control for the whole geometry gate).
void test_boot_image__minimal_valid_geometry_accepts(void) {
  cl_assert(boot_image_geometry_valid(FW_DESCRIPTION_LENGTH, 1, TEST_SLOT_CAPACITY));
}

// -----------------------------------------------------------------------------
// CRC gate (pure decision fed by the real crc32 over a payload buffer)

// The canonical CRC-32 "check" value of the 9 bytes "123456789" is 0xCBF43926
// (A Painless Guide to CRC Error Detection Algorithms). A descriptor carrying
// that checksum accepts.
void test_boot_image__checksum_match_accepts(void) {
  const uint8_t payload[] = "123456789";
  uint32_t computed = crc32(0, payload, 9);
  cl_assert_equal_i(computed, 0xCBF43926u);
  cl_assert(boot_image_checksum_valid(computed, 0xCBF43926u));
}

// A descriptor whose stored checksum does NOT match the payload's CRC is
// rejected (the corrupt-image gate). Feeds the real CRC but a wrong expected
// value.
void test_boot_image__checksum_mismatch_rejects(void) {
  const uint8_t payload[] = "123456789";
  uint32_t computed = crc32(0, payload, 9);
  cl_assert_equal_i(computed, 0xCBF43926u);
  cl_assert(!boot_image_checksum_valid(computed, 0xCBF43926u ^ 0x1u));
}
