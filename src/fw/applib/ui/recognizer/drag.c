/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "drag.h"

#include "recognizer.h"
#include "recognizer_impl.h"

#include "applib/graphics/gtypes.h"

#include "pbl/util/math.h"

#include <string.h>

typedef struct DragRecognizerData {
  // Recognizer config
  struct {
    int16_t start_threshold_px;
    uint8_t min_updates;
  } config;

  // Gesture state
  struct {
    GPoint start;
    GPoint last;
    int32_t cum_dx;
    int32_t cum_dy;
    int16_t event_dy;
    uint8_t updates;
  } state;
} DragRecognizerData;

static void prv_handle_touch_event(Recognizer *recognizer, const TouchEvent *touch_event);
static bool prv_cancel(Recognizer *recognizer);
static void prv_reset(Recognizer *recognizer);

static const RecognizerImpl s_drag_recognizer_impl = {
    .handle_touch_event = prv_handle_touch_event,
    .cancel = prv_cancel,
    .reset = prv_reset,
};

static void prv_handle_touch_event(Recognizer *recognizer, const TouchEvent *touch_event) {
  DragRecognizerData *data = recognizer_get_impl_data(recognizer, &s_drag_recognizer_impl);
  const RecognizerState state = recognizer_get_state(recognizer);

  switch (touch_event->type) {
    case TouchEvent_Touchdown:
      data->state.start = GPoint(touch_event->x, touch_event->y);
      data->state.last = data->state.start;
      break;

    case TouchEvent_PositionUpdate: {
      const int16_t dy = touch_event->y - data->state.last.y;
      data->state.cum_dx += touch_event->x - data->state.last.x;
      data->state.cum_dy += dy;
      data->state.last = GPoint(touch_event->x, touch_event->y);
      if (data->state.updates < data->config.min_updates) {
        // Saturate: the count gate only needs min_updates, and a long
        // low-motion touch must not wrap the counter
        data->state.updates++;
      }

      if (state != RecognizerState_Possible) {
        data->state.event_dy = dy;
        recognizer_transition_state(recognizer, RecognizerState_Updated);
      } else if ((data->state.updates >= data->config.min_updates) &&
                 (ABS(data->state.cum_dy) >= data->config.start_threshold_px)) {
        // ORDERING CONSTRAINT: this ratio assumes DE-SHEARED input. Wiring
        // this recognizer to a delivery path still carrying raw chip
        // coordinates inverts it (physical horizontal strokes start drags;
        // physical vertical strokes sit near the fail boundary): the
        // de-shear must land on the delivery path before anything attaches
        // this recognizer.
        if (ABS(data->state.cum_dx) >= DRAG_HORIZONTAL_FAIL_RATIO * ABS(data->state.cum_dy)) {
          // Horizontal-dominant stroke falls through to gesture emulation;
          // same constant and inclusivity as the driver's dominance gate, so
          // both resolve the 3:1 boundary consistently once they share a
          // coordinate frame (see DRAG_HORIZONTAL_FAIL_RATIO)
          recognizer_transition_state(recognizer, RecognizerState_Failed);
        } else {
          // Deliver everything accumulated so far minus the signed start
          // threshold: the threshold is absorbed as slop (no snap at drag
          // start) and any larger pre-start travel -- unbounded on sparse
          // strokes, where one update can carry 30+ px -- is preserved
          data->state.event_dy =
              data->state.cum_dy - ((data->state.cum_dy > 0) ? data->config.start_threshold_px
                                                             : -data->config.start_threshold_px);
          recognizer_transition_state(recognizer, RecognizerState_Started);
        }
      }
      break;
    }

    case TouchEvent_Liftoff:
      if (state == RecognizerState_Possible) {
        recognizer_transition_state(recognizer, RecognizerState_Failed);
      } else {
        // Liftoff coordinates are unreliable and are never read
        data->state.event_dy = 0;
        recognizer_transition_state(recognizer, RecognizerState_Completed);
      }
      break;
  }
}

static bool prv_cancel(Recognizer *recognizer) {
  DragRecognizerData *data = recognizer_get_impl_data(recognizer, &s_drag_recognizer_impl);
  // Only invoked mid-gesture (Started/Updated): fire the Cancelled event
  // with a zero delta, symmetric with liftoff -- a consumer applying
  // delta_y unconditionally must not scroll on cancel
  data->state.event_dy = 0;
  return true;
}

static void prv_reset(Recognizer *recognizer) {
  DragRecognizerData *data = recognizer_get_impl_data(recognizer, &s_drag_recognizer_impl);
  memset(&data->state, 0, sizeof(data->state));
}

Recognizer *drag_recognizer_create(RecognizerEventCb event_cb, void *user_data) {
  DragRecognizerData data = {
      .config =
          {
              .start_threshold_px = DRAG_START_THRESHOLD_PX,
              .min_updates = DRAG_MIN_UPDATES,
          },
  };

  return recognizer_create_with_data(&s_drag_recognizer_impl, &data, sizeof(data), event_cb,
                                     user_data);
}

int16_t drag_recognizer_get_delta_y(const Recognizer *recognizer) {
  DragRecognizerData *data =
      recognizer_get_impl_data((Recognizer *)recognizer, &s_drag_recognizer_impl);
  if (!data) {
    return 0;
  }
  return data->state.event_dy;
}
