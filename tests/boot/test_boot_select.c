/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "boot_select.h"

// No fixtures needed; boot_select() is pure.
void test_boot_select__initialize(void) {}
void test_boot_select__cleanup(void) {}

// An untrusted retained page (CRC failed) must never trigger a copy, whatever
// the (garbage) boot bits say: start the resident firmware.
void test_boot_select__untrusted_page_never_copies(void) {
  BootPlan p = boot_select(BOOT_BIT_FORCE_PRF | BOOT_BIT_NEW_FW_AVAILABLE, false);
  cl_assert(!p.want_copy);
}

// No boot bits set -> chain-load the resident image.
void test_boot_select__no_bits_no_copy(void) {
  BootPlan p = boot_select(0, true);
  cl_assert(!p.want_copy);
}

// FORCE_PRF selects the PRF from SAFE_FIRMWARE with the recovery strike bits.
void test_boot_select__force_prf(void) {
  BootPlan p = boot_select(BOOT_BIT_FORCE_PRF, true);
  cl_assert(p.want_copy);
  cl_assert_equal_i(p.src_addr, NOR_SAFE_FIRMWARE);
  cl_assert_equal_i(p.consume_bit, BOOT_BIT_FORCE_PRF);
  cl_assert_equal_i(p.strike_one, BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_ONE);
  cl_assert_equal_i(p.strike_two, BOOT_BIT_RECOVERY_LOAD_FAIL_STRIKE_TWO);
  cl_assert_equal_i(p.install_bit, 0);
}

// NEW_FW_AVAILABLE selects the staging slot with the fw-start strike bits and
// the NEW_FW_INSTALLED marker.
void test_boot_select__new_fw(void) {
  BootPlan p = boot_select(BOOT_BIT_NEW_FW_AVAILABLE, true);
  cl_assert(p.want_copy);
  cl_assert_equal_i(p.src_addr, NOR_FIRMWARE_SLOT_1);
  cl_assert_equal_i(p.consume_bit, BOOT_BIT_NEW_FW_AVAILABLE);
  cl_assert_equal_i(p.strike_one, BOOT_BIT_FW_START_FAIL_STRIKE_ONE);
  cl_assert_equal_i(p.strike_two, BOOT_BIT_FW_START_FAIL_STRIKE_TWO);
  cl_assert_equal_i(p.install_bit, BOOT_BIT_NEW_FW_INSTALLED);
}

// When BOTH are set, an explicit recovery request wins over a staged update.
void test_boot_select__force_prf_beats_new_fw(void) {
  BootPlan p = boot_select(BOOT_BIT_FORCE_PRF | BOOT_BIT_NEW_FW_AVAILABLE, true);
  cl_assert(p.want_copy);
  cl_assert_equal_i(p.src_addr, NOR_SAFE_FIRMWARE);
  cl_assert_equal_i(p.consume_bit, BOOT_BIT_FORCE_PRF);
}

// Strike escalation: first failure -> strike_one; if strike_one already set,
// escalate to strike_two.
void test_boot_select__strike_escalation(void) {
  BootPlan p = boot_select(BOOT_BIT_FORCE_PRF, true);
  // no strikes yet -> strike_one
  cl_assert_equal_i(boot_next_strike(&p, BOOT_BIT_FORCE_PRF), p.strike_one);
  // strike_one already present -> strike_two
  cl_assert_equal_i(boot_next_strike(&p, BOOT_BIT_FORCE_PRF | p.strike_one), p.strike_two);
}
