/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "applib/ui/recognizer/drag.h"
#include "applib/ui/recognizer/recognizer.h"
#include "applib/ui/recognizer/recognizer_private.h"

// Stubs
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"

void recognizer_manager_handle_state_change(RecognizerManager *manager, Recognizer *changed) {}

// Subscriber event log: every event with the delta_y observed at delivery time
#define MAX_EVENTS 16
typedef struct EventLogEntry {
  RecognizerEvent type;
  int16_t delta_y;
} EventLogEntry;

static EventLogEntry s_events[MAX_EVENTS];
static unsigned s_num_events;

static void prv_event_cb(const Recognizer *recognizer, RecognizerEvent event_type) {
  cl_assert(s_num_events < MAX_EVENTS);
  s_events[s_num_events] = (EventLogEntry){
      .type = event_type,
      .delta_y = drag_recognizer_get_delta_y(recognizer),
  };
  s_num_events++;
}

static void prv_assert_event_log(const EventLogEntry *expected, unsigned count) {
  cl_assert_equal_i(s_num_events, count);
  for (unsigned i = 0; i < count; i++) {
    cl_assert_equal_i(s_events[i].type, expected[i].type);
    cl_assert_equal_i(s_events[i].delta_y, expected[i].delta_y);
  }
}

static void prv_send(Recognizer *r, TouchEventType type, int16_t x, int16_t y) {
  recognizer_handle_touch_event(r, &(TouchEvent){.type = type, .x = x, .y = y});
}

static Recognizer *s_recognizer;

// setup and teardown
void test_drag__initialize(void) {
  s_num_events = 0;
  s_recognizer = drag_recognizer_create(prv_event_cb, NULL);
  cl_assert(s_recognizer != NULL);
}

void test_drag__cleanup(void) {
  recognizer_destroy(s_recognizer);
  s_recognizer = NULL;
}

// tests

// Boundary decision: exactly 8 px of cumulative |dy| with 2 updates STARTS the drag
// (threshold is inclusive, ">= 8": the spec requires "at least 8 framebuffer px").
void test_drag__happy_path(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  cl_assert_equal_i(s_num_events, 0);

  // Update 1: cum_dy = 4, updates = 1 -> below both gates, still Possible
  prv_send(r, TouchEvent_PositionUpdate, 50, 54);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  cl_assert_equal_i(s_num_events, 0);

  // Update 2: cum_dy = 8 (exactly the threshold), updates = 2 -> Started
  // delivering the accumulated travel minus the signed threshold: 8 - 8 = 0
  prv_send(r, TouchEvent_PositionUpdate, 50, 58);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  // Updates 3-5: Updated with per-event dy
  prv_send(r, TouchEvent_PositionUpdate, 50, 62);
  prv_send(r, TouchEvent_PositionUpdate, 50, 66);
  prv_send(r, TouchEvent_PositionUpdate, 50, 70);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Updated);

  prv_send(r, TouchEvent_Liftoff, 50, 70);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Completed);

  const EventLogEntry expected[] = {
      {.type = RecognizerEvent_Started, .delta_y = 0},
      {.type = RecognizerEvent_Updated, .delta_y = 4},
      {.type = RecognizerEvent_Updated, .delta_y = 4},
      {.type = RecognizerEvent_Updated, .delta_y = 4},
      {.type = RecognizerEvent_Completed, .delta_y = 0},
  };
  prv_assert_event_log(expected, 5);

  // Injected total dy = 20; exactly the 8 px start threshold is absorbed as
  // slop, so delivered deltas sum to 12 regardless of how the pre-Started
  // travel was distributed across updates
  int32_t delivered = 0;
  for (unsigned i = 0; i < s_num_events; i++) {
    if ((s_events[i].type == RecognizerEvent_Started) ||
        (s_events[i].type == RecognizerEvent_Updated)) {
      delivered += s_events[i].delta_y;
    }
  }
  cl_assert_equal_i(delivered, 20 - 8);
}

// A sparse stroke (frame drops) can carry huge per-update deltas. The first
// update's travel fails only the update-count gate, so it must NOT be
// discarded: 30 + 25 = 55 px accumulated, minus the 8 px threshold = 47
// delivered at Started.
void test_drag__sparse_stroke_first_update_preserved(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // Update 1: dy = 30, distance gate satisfied, count gate is not
  prv_send(r, TouchEvent_PositionUpdate, 50, 80);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  cl_assert_equal_i(s_num_events, 0);

  // Update 2: dy = 25, cum_dy = 55 -> Started with 55 - 8 = 47
  prv_send(r, TouchEvent_PositionUpdate, 50, 105);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  prv_send(r, TouchEvent_Liftoff, 50, 105);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Completed);

  const EventLogEntry expected[] = {
      {.type = RecognizerEvent_Started, .delta_y = 47},
      {.type = RecognizerEvent_Completed, .delta_y = 0},
  };
  prv_assert_event_log(expected, 2);
}

// The start gate is re-evaluated on every update: a slow stroke that first
// crosses the 8 px threshold at update 3 starts there, not only at update 2.
void test_drag__starts_at_third_update(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // Updates 1-2: cum_dy = 4, count gate satisfied, distance gate is not
  prv_send(r, TouchEvent_PositionUpdate, 50, 52);
  prv_send(r, TouchEvent_PositionUpdate, 50, 54);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  cl_assert_equal_i(s_num_events, 0);

  // Update 3: dy = 5, cum_dy = 9 -> Started with 9 - 8 = 1
  prv_send(r, TouchEvent_PositionUpdate, 50, 59);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  prv_send(r, TouchEvent_Liftoff, 50, 59);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Completed);

  const EventLogEntry expected[] = {
      {.type = RecognizerEvent_Started, .delta_y = 1},
      {.type = RecognizerEvent_Completed, .delta_y = 0},
  };
  prv_assert_event_log(expected, 2);
}

void test_drag__horizontal_dominant_fails(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // At threshold evaluation: cum_dx = 30, cum_dy = 10 >= 8, updates = 2,
  // |cum_dx| >= 3 * |cum_dy| (exactly 3:1, matching the cst816 swipe
  // classifier's inclusive dominance gate) -> Failed, no events fired
  prv_send(r, TouchEvent_PositionUpdate, 65, 55);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  prv_send(r, TouchEvent_PositionUpdate, 80, 60);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Failed);
  cl_assert_equal_i(s_num_events, 0);
}

// A diagonal below 3:1 horizontal dominance must DRAG, not fail: the driver
// treats >=3:1 dominance as a horizontal swipe and everything below it as
// vertical, so the drag must claim sub-3:1 strokes rather than fail them.
void test_drag__diagonal_below_swipe_ratio_drags(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // cum_dx = 20, cum_dy = 10: 2:1 horizontal, below the fail ratio
  // -> Started with 10 - 8 = 2
  prv_send(r, TouchEvent_PositionUpdate, 60, 55);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  prv_send(r, TouchEvent_PositionUpdate, 70, 60);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  prv_send(r, TouchEvent_Liftoff, 70, 60);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Completed);

  const EventLogEntry expected[] = {
      {.type = RecognizerEvent_Started, .delta_y = 2},
      {.type = RecognizerEvent_Completed, .delta_y = 0},
  };
  prv_assert_event_log(expected, 2);
}

// |cum_dx| == |cum_dy| tie: 1:1 is far below the 3:1 fail ratio, so a
// perfect diagonal resolves to a drag.
void test_drag__equal_dx_dy_tie_drags(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // cum_dx = 10 == cum_dy = 10 -> Started with 10 - 8 = 2
  prv_send(r, TouchEvent_PositionUpdate, 55, 55);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  prv_send(r, TouchEvent_PositionUpdate, 60, 60);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  prv_send(r, TouchEvent_Liftoff, 60, 60);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Completed);

  const EventLogEntry expected[] = {
      {.type = RecognizerEvent_Started, .delta_y = 2},
      {.type = RecognizerEvent_Completed, .delta_y = 0},
  };
  prv_assert_event_log(expected, 2);
}

void test_drag__too_few_updates_never_starts(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // One update of dy = 20: distance gate satisfied, min_updates floor is not
  prv_send(r, TouchEvent_PositionUpdate, 50, 70);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  cl_assert_equal_i(s_num_events, 0);

  prv_send(r, TouchEvent_Liftoff, 50, 70);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Failed);
  cl_assert_equal_i(s_num_events, 0);
}

void test_drag__below_threshold_never_starts(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // Two updates totaling |dy| = 4 < 8: update count satisfied, distance is not
  prv_send(r, TouchEvent_PositionUpdate, 50, 52);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  prv_send(r, TouchEvent_PositionUpdate, 50, 54);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);

  // Third update, cum |dy| = 6, still below the 8 px threshold
  prv_send(r, TouchEvent_PositionUpdate, 50, 56);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  cl_assert_equal_i(s_num_events, 0);

  prv_send(r, TouchEvent_Liftoff, 50, 56);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Failed);
  cl_assert_equal_i(s_num_events, 0);
}

void test_drag__liftoff_coords_ignored(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // Two updates of dy = +5 each: cum_dy = 10 > 8 -> Started with 10 - 8 = 2
  prv_send(r, TouchEvent_PositionUpdate, 50, 55);
  prv_send(r, TouchEvent_PositionUpdate, 50, 60);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  // Garbage liftoff coordinates must never be read: Completed delivers delta_y = 0
  prv_send(r, TouchEvent_Liftoff, 9999, -9999);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Completed);
  cl_assert_equal_i(drag_recognizer_get_delta_y(r), 0);

  const EventLogEntry expected[] = {
      {.type = RecognizerEvent_Started, .delta_y = 2},
      {.type = RecognizerEvent_Completed, .delta_y = 0},
  };
  prv_assert_event_log(expected, 2);
}

void test_drag__cancel_mid_drag(void) {
  Recognizer *r = s_recognizer;
  prv_send(r, TouchEvent_Touchdown, 50, 50);

  // Upward drag: two updates of dy = -6 each, cum_dy = -12, |cum_dy| >= 8
  // -> Started with -12 - (-8) = -4 (the signed threshold is absorbed)
  prv_send(r, TouchEvent_PositionUpdate, 50, 44);
  prv_send(r, TouchEvent_PositionUpdate, 50, 38);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  recognizer_cancel(r);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Cancelled);

  const EventLogEntry cancelled[] = {
      {.type = RecognizerEvent_Started, .delta_y = -4},
      // Cancelled delivers 0, symmetric with liftoff: a consumer applying
      // delta_y unconditionally must not scroll on cancel
      {.type = RecognizerEvent_Cancelled, .delta_y = 0},
  };
  prv_assert_event_log(cancelled, 2);

  // Reset zeroes gesture state but keeps config: a fresh gesture must re-arm
  // from scratch (stale cum/updates would start on the first update below)
  recognizer_reset(r);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  cl_assert_equal_i(drag_recognizer_get_delta_y(r), 0);

  s_num_events = 0;
  prv_send(r, TouchEvent_Touchdown, 10, 10);
  prv_send(r, TouchEvent_PositionUpdate, 10, 14);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  prv_send(r, TouchEvent_PositionUpdate, 10, 18);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);
  prv_send(r, TouchEvent_Liftoff, 10, 18);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Completed);

  const EventLogEntry after_reset[] = {
      {.type = RecognizerEvent_Started, .delta_y = 0},
      {.type = RecognizerEvent_Completed, .delta_y = 0},
  };
  prv_assert_event_log(after_reset, 2);
}
