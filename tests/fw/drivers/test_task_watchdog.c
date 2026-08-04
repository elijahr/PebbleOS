/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

// Light header stubs that do not collide with each other.
#include "stubs_freertos.h"
#include "stubs_logging.h"
#include "stubs_new_timer.h"
#include "stubs_passert.h"
#include "stubs_pebble_tasks.h"

#include "drivers/task_watchdog.h"
#include "system/reboot_reason.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>

// Mockable hardware feed: count reloads instead of touching NRF_WDT.
static int s_feed_count;
void watchdog_feed(void) { s_feed_count++; }

// -------------------------------------------------------------------------------------------------
// Minimal definitions for the rest of the real task_watchdog.c dependency surface. None of these
// paths run in the tests below; the linker only needs the symbols to resolve.
void reboot_reason_get(RebootReason *reason) {}
void reboot_reason_set(RebootReason *reason) {}
void reboot_reason_clear(void) {}

void *system_task_get_current_callback(void) { return NULL; }
bool system_task_is_ready_to_run(void) { return true; }

bool event_put_isr(void *event) { return true; }

void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority) {}
uintptr_t ulTaskDebugGetStackedPC(TaskHandle_t task) { return 0; }
uintptr_t ulTaskDebugGetStackedLR(TaskHandle_t task) { return 0; }

void test_task_watchdog__initialize(void) {
  s_feed_count = 0;
  task_watchdog_init();
}

// Ungated: the startup feed reloads the HW WDT even with NO task check-in.
void test_task_watchdog__startup_feed_is_ungated(void) {
  cl_assert_equal_i(s_feed_count, 0);
  task_watchdog_startup_feed();
  cl_assert_equal_i(s_feed_count, 1);  // fed with zero tasks checked in
}

// Every startup tick before handover feeds (middle-window coverage).
void test_task_watchdog__startup_feed_every_tick(void) {
  s_feed_count = 0;
  task_watchdog_startup_feed();
  task_watchdog_startup_feed();
  task_watchdog_startup_feed();
  cl_assert_equal_i(s_feed_count, 3);
}

// After the single default-mask task checks in, the gated path owns the WDT and
// the ungated startup feed becomes a no-op (clean handover, single owner).
void test_task_watchdog__stops_after_handover(void) {
  s_feed_count = 0;
  // The default mask is a single task: DEFAULT_TASK_WATCHDOG_MASK ==
  // (1 << PebbleTask_NewTimers) (task_watchdog.c:40). Check that one task in,
  // then a gated feed, so the mask-satisfied branch runs and sets
  // s_hw_watchdog_taken_over.
  task_watchdog_bit_set(PebbleTask_NewTimers);
  task_watchdog_feed();  // gated feed with full mask -> handover (exactly 1 HW feed)
  cl_assert_equal_i(s_feed_count, 1);
  task_watchdog_startup_feed();
  cl_assert_equal_i(s_feed_count, 1);  // no extra ungated feed after handover
}
