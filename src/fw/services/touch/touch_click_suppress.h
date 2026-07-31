/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "drivers/rtc.h"
#include "pbl/services/touch/touch_event.h"

#include <stdbool.h>

//! Per-stroke synthetic-click suppression. When a touch stroke is consumed by
//! a widget (e.g. drag-to-scroll actually moved content), the emulated click
//! the driver synthesizes for the same stroke must be dropped at the click
//! dispatch site. The whole module fails OPEN: if state is uncertain, the
//! click passes — the app queue drops events silently in release builds, so a
//! fail-closed design would eat real clicks.

#if CONFIG_TOUCH_NAV_BUTTONS

//! Provisional until the S7 silicon gate. Measured intentional-swipe cadence
//! floor is 352 ms, ~2.3x this window; the driver's own coalesce window is
//! 50 ms and can emit intermediate Liftoff/Touchdown pairs inside one
//! physical stroke, which is why Touchdown only clears past this gap.
#define TOUCH_STROKE_CONTINUATION_MS 150

//! Per-task suppression state; the app task's instance lives in AppState.
typedef struct TouchClickSuppressState {
  bool consumed;
  RtcTicks last_liftoff_ticks;
} TouchClickSuppressState;

//! Mark the current stroke consumed: its synthetic click must be dropped.
//! Called by widgets on a real outcome (e.g. the scroll offset moved).
void touch_click_suppress_mark_consumed(void);

//! Feed dispatched touch events to the stroke-boundary state machine:
//! Liftoff records dequeue ticks; Touchdown clears a stale consumed flag only
//! when the gap since Liftoff exceeds TOUCH_STROKE_CONTINUATION_MS.
void touch_click_suppress_note_touch_event(const TouchEvent *event);

//! @return true if the pending synthetic click belongs to a consumed stroke
//! and must be dropped. Returning true clears the flag (one click per mark).
bool touch_click_suppress_should_drop_click(void);

#else  // no-op stubs for every CONFIG_TOUCH_NAV_BUTTONS=n consumer

static inline void touch_click_suppress_mark_consumed(void) {}
static inline void touch_click_suppress_note_touch_event(const TouchEvent *event) {}
static inline bool touch_click_suppress_should_drop_click(void) {
  return false;
}

#endif
