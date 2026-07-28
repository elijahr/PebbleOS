/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "services/touch/touch_click_suppress.h"

#if CONFIG_TOUCH_NAV_BUTTONS

#include "kernel/pebble_tasks.h"
#include "pbl/os/tick.h"
#include "process_state/app_state/app_state.h"

// KernelMain slot. No caller marks this flag yet, so the KernelMain filter in
// kernel/event_loop.c is a no-op until one exists (e.g. a modal widget
// consuming a drag).
static TouchClickSuppressState s_kernel_state;

//! Per-task state selection, mirroring touch_service.c. Any task without a
//! state slot gets NULL and every entry point fails open.
static TouchClickSuppressState *prv_get_state(void) {
  switch (pebble_task_get_current()) {
    case PebbleTask_App:
      return app_state_get_touch_click_suppress_state();
    case PebbleTask_KernelMain:
      return &s_kernel_state;
    default:
      return NULL;
  }
}

void touch_click_suppress_mark_consumed(void) {
  TouchClickSuppressState *state = prv_get_state();
  if (!state) {
    return;
  }
  state->consumed = true;
}

void touch_click_suppress_note_touch_event(const TouchEvent *event) {
  TouchClickSuppressState *state = prv_get_state();
  if (!state || !event) {
    return;
  }
  switch (event->type) {
    case TouchEvent_Liftoff:
      // Dequeue-time ticks: this runs on the dispatching task, after the
      // event crossed the app queue, which is the same clock the eventual
      // synthetic click experiences.
      state->last_liftoff_ticks = rtc_get_ticks();
      break;
    case TouchEvent_Touchdown:
      // Clear a stale consumed flag only for a genuinely NEW stroke. The
      // driver coalesces strokes within ~50 ms and can emit intermediate
      // Liftoff/Touchdown pairs inside one physical stroke; clearing on
      // every Touchdown would reset mid-drag.
      if (state->consumed && (rtc_get_ticks() - state->last_liftoff_ticks >
                              milliseconds_to_ticks(TOUCH_STROKE_CONTINUATION_MS))) {
        state->consumed = false;
      }
      break;
    default:
      break;
  }
}

bool touch_click_suppress_should_drop_click(void) {
  TouchClickSuppressState *state = prv_get_state();
  if (!state) {
    return false;
  }
  if (!state->consumed) {
    return false;
  }
  // One click per mark: dropping clears the flag.
  state->consumed = false;
  return true;
}

#endif  // CONFIG_TOUCH_NAV_BUTTONS
