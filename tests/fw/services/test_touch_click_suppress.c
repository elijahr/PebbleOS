/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "kernel/pebble_tasks.h"
#include "pbl/services/touch/touch_event.h"
#include "services/touch/touch_click_suppress.h"

#include <stdbool.h>

#include "fake_pebble_tasks.h"
#include "fake_rtc.h"

// Stubs
#include "stubs_app_state.h"
#include "stubs_tick.h"

static void prv_note(TouchEventType type) {
  TouchEvent event = {.type = type, .x = 0, .y = 0};
  touch_click_suppress_note_touch_event(&event);
}

// setup and teardown
void test_touch_click_suppress__initialize(void) {
  fake_rtc_init(1000 /* ticks */, 0 /* time */);
  stub_pebble_tasks_set_current(PebbleTask_App);
  *app_state_get_touch_click_suppress_state() = (TouchClickSuppressState){};
}

void test_touch_click_suppress__cleanup(void) {}

// tests

// S4 (a): mark sets the flag; a drop clears it so only one click is eaten.
void test_touch_click_suppress__mark_then_drop_clears(void) {
  touch_click_suppress_mark_consumed();
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), true);
  // The drop itself must clear the flag: the next click passes.
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}

// S4 (b): fail-open. Fresh state never drops; unknown-task state is inert and
// never leaks into the app slot.
void test_touch_click_suppress__fail_open(void) {
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);

  // Worker task has no suppression state: every entry point must no-op
  // (fail-open), not crash and not mark anything.
  stub_pebble_tasks_set_current(PebbleTask_Worker);
  touch_click_suppress_mark_consumed();
  prv_note(TouchEvent_Liftoff);
  prv_note(TouchEvent_Touchdown);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);

  // Back on the app task: the worker-side mark must not have touched app state.
  stub_pebble_tasks_set_current(PebbleTask_App);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}

// S4 (c), design trace 2: the driver's coalescing can emit an INTERMEDIATE
// Liftoff/Touchdown pair inside one physical stroke. A Touchdown within the
// continuation window must NOT clear the consumed flag.
void test_touch_click_suppress__trace2_continuation_inside_window(void) {
  touch_click_suppress_mark_consumed();
  prv_note(TouchEvent_Liftoff);
  fake_rtc_increment_ticks(milliseconds_to_ticks(50));
  prv_note(TouchEvent_Touchdown);
  // Gap 50 ms <= TOUCH_STROKE_CONTINUATION_MS (150): same stroke, flag holds.
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), true);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}

// S4 (d), design trace 3: a Touchdown past the continuation window is a new
// stroke; the stale consumed flag clears so the new stroke's click passes.
void test_touch_click_suppress__trace3_new_stroke_clears(void) {
  touch_click_suppress_mark_consumed();
  prv_note(TouchEvent_Liftoff);
  fake_rtc_increment_ticks(milliseconds_to_ticks(200));
  prv_note(TouchEvent_Touchdown);
  // Gap 200 ms > 150 ms: new stroke, flag cleared, click passes.
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}

// Boundary pin: the implementation clears only when the Liftoff->Touchdown
// gap STRICTLY exceeds TOUCH_STROKE_CONTINUATION_MS, so exactly 150 ms is
// still a continuation (flag holds). 149/150/151 pin both sides so a
// mutation flipping > to >= cannot survive.
static bool prv_drop_after_gap_ms(uint32_t gap_ms) {
  touch_click_suppress_mark_consumed();
  prv_note(TouchEvent_Liftoff);
  fake_rtc_increment_ticks(milliseconds_to_ticks(gap_ms));
  prv_note(TouchEvent_Touchdown);
  const bool dropped = touch_click_suppress_should_drop_click();
  // Drain: a drop clears; a non-drop means Touchdown already cleared.
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
  return dropped;
}

void test_touch_click_suppress__boundary_149ms_holds(void) {
  cl_assert_equal_b(prv_drop_after_gap_ms(149), true);
}

void test_touch_click_suppress__boundary_150ms_holds(void) {
  // Exactly TOUCH_STROKE_CONTINUATION_MS: continuation side, flag holds.
  cl_assert_equal_b(prv_drop_after_gap_ms(150), true);
}

void test_touch_click_suppress__boundary_151ms_clears(void) {
  cl_assert_equal_b(prv_drop_after_gap_ms(151), false);
}

// KernelMain owns its own state slot, independent of the app slot: a mark on
// KernelMain must never drop an app-task click, and vice versa.
void test_touch_click_suppress__kernel_main_slot_independent(void) {
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);
  touch_click_suppress_mark_consumed();

  // The app slot must be untouched by the KernelMain mark.
  stub_pebble_tasks_set_current(PebbleTask_App);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);

  // KernelMain drops its own marked click, exactly once.
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), true);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}

// S4 (e), design trace 4: consumed stroke whose click never arrived, then a
// tap at the measured intentional cadence floor (352 ms >> 150 ms window).
// The stale flag must not eat the tap's click.
void test_touch_click_suppress__trace4_no_click_stroke_then_tap(void) {
  touch_click_suppress_mark_consumed();
  prv_note(TouchEvent_Liftoff);
  // No click was dispatched for the consumed stroke (e.g. classifier emitted
  // no gesture). The flag is stale by the time the user taps again.
  fake_rtc_increment_ticks(milliseconds_to_ticks(400));
  prv_note(TouchEvent_Touchdown);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
  // The tap's own liftoff and click follow: still nothing dropped.
  prv_note(TouchEvent_Liftoff);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}
