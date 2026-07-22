/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/touch/touch.h"
#include "pbl/services/touch/touch_event.h"

#include "drivers/button_id.h"
#include "drivers/display/display.h"
#include "drivers/touch/touch_sensor.h"
#include "kernel/events.h"
#include "kernel/pebble_tasks.h"
#include "pbl/services/event_service.h"
#include "pbl/services/analytics/analytics.h"
#include "syscall/syscall.h"
#include "syscall/syscall_internal.h"
#include "system/logging.h"
#include "pbl/os/mutex.h"
#include "system/passert.h"

PBL_LOG_MODULE_DEFINE(service_touch, CONFIG_SERVICE_TOUCH_LOG_LEVEL);

static TouchState s_touch_state = TouchState_FingerUp;
static int16_t s_last_x;
static int16_t s_last_y;

static PebbleMutex *s_touch_mutex;

static uint8_t s_subscriber_count = 0;
static bool s_backlight_subscribed = false;
static bool s_globally_enabled = true;
static bool s_rotated = false;

static void prv_apply_rotation(int16_t *x, int16_t *y) {
  if (s_rotated) {
    *x = (DISP_COLS - 1) - *x;
    *y = (DISP_ROWS - 1) - *y;
  }
}

static void prv_add_subscriber_cb(PebbleTask task) {
  mutex_lock(s_touch_mutex);
  // Honor the global kill switch: when touch is globally disabled, track the
  // subscriber count but don't power up the sensor.
  if (++s_subscriber_count == 1 && s_globally_enabled) {
    touch_sensor_set_enabled(true);
  }
  mutex_unlock(s_touch_mutex);
}

static void prv_remove_subscriber_cb(PebbleTask task) {
  mutex_lock(s_touch_mutex);
  PBL_ASSERTN(s_subscriber_count > 0);
  if (--s_subscriber_count == 0 && s_globally_enabled) {
    touch_sensor_set_enabled(false);
  }
  mutex_unlock(s_touch_mutex);
}

void touch_init(void) {
  s_touch_mutex = mutex_create();

  event_service_init(PEBBLE_TOUCH_EVENT, &prv_add_subscriber_cb,
      &prv_remove_subscriber_cb);
  event_service_init(PEBBLE_GESTURE_EVENT, &prv_add_subscriber_cb,
      &prv_remove_subscriber_cb);
}

bool touch_has_app_subscribers(void) {
  mutex_lock(s_touch_mutex);
  // The backlight gesture subscription is tracked in s_subscriber_count as
  // well; exclude it so this only reflects real app subscribers.
  const uint8_t backlight_count = s_backlight_subscribed ? 1 : 0;
  const bool has_apps = s_subscriber_count > backlight_count;
  mutex_unlock(s_touch_mutex);
  return has_apps;
}

void touch_service_set_globally_enabled(bool enabled) {
  mutex_lock(s_touch_mutex);
  if (s_globally_enabled == enabled) {
    mutex_unlock(s_touch_mutex);
    return;
  }
  s_globally_enabled = enabled;
  const bool sensor_enabled = enabled && (s_subscriber_count > 0);
  mutex_unlock(s_touch_mutex);

  touch_sensor_set_enabled(sensor_enabled);
  if (!enabled) {
    // Avoid delivering stale position on re-enable.
    touch_reset();
  }
}

bool touch_service_is_globally_enabled(void) {
  mutex_lock(s_touch_mutex);
  const bool enabled = s_globally_enabled;
  mutex_unlock(s_touch_mutex);
  return enabled;
}

DEFINE_SYSCALL(bool, sys_touch_service_is_enabled, void) {
  return touch_service_is_globally_enabled();
}

DEFINE_SYSCALL(void, sys_touch_reset, void) {
  touch_reset();
}

void touch_set_backlight_enabled(bool enabled) {
  mutex_lock(s_touch_mutex);
  if (enabled && !s_backlight_subscribed) {
    s_backlight_subscribed = true;
    mutex_unlock(s_touch_mutex);
    prv_add_subscriber_cb(PebbleTask_KernelMain);
    return;
  } else if (!enabled && s_backlight_subscribed) {
    s_backlight_subscribed = false;
    mutex_unlock(s_touch_mutex);
    prv_remove_subscriber_cb(PebbleTask_KernelMain);
    return;
  }
  mutex_unlock(s_touch_mutex);
}

static void prv_put_touch_event(TouchEventType type, int16_t x, int16_t y) {
  PebbleEvent e = {
    .type = PEBBLE_TOUCH_EVENT,
    .touch = {
      .event = {
        .type = type,
        .x = x,
        .y = y,
      },
    },
  };
  event_put(&e);
}

static void prv_put_gesture_event(GestureEventType gesture, int16_t x, int16_t y) {
  PebbleEvent e = {
    .type = PEBBLE_GESTURE_EVENT,
    .gesture = {
      .event = {
        .type = gesture,
        .x = x,
        .y = y,
      },
    },
  };
  event_put(&e);
}

void touch_handle_update(TouchState touch_state, int16_t x, int16_t y) {
  mutex_lock(s_touch_mutex);

  if (!s_globally_enabled) {
    mutex_unlock(s_touch_mutex);
    return;
  }

  prv_apply_rotation(&x, &y);

  if (s_touch_state != touch_state) {
    s_touch_state = touch_state;
    s_last_x = x;
    s_last_y = y;
    mutex_unlock(s_touch_mutex);

    if (touch_state == TouchState_FingerDown) {
      PBL_ANALYTICS_ADD(touch_event_count, 1);
      PBL_LOG_DBG("Touch: Touchdown @ (%" PRId16 ", %" PRId16 ")", x, y);
      prv_put_touch_event(TouchEvent_Touchdown, x, y);
    } else {
      PBL_LOG_DBG("Touch: Liftoff");
      prv_put_touch_event(TouchEvent_Liftoff, x, y);
    }
    return;
  }

  if (touch_state == TouchState_FingerDown && (x != s_last_x || y != s_last_y)) {
    s_last_x = x;
    s_last_y = y;
    mutex_unlock(s_touch_mutex);

    PBL_LOG_DBG("Touch: Position Update @ (%" PRId16 ", %" PRId16 ")", x, y);
    prv_put_touch_event(TouchEvent_PositionUpdate, x, y);
    return;
  }

  mutex_unlock(s_touch_mutex);
}

// Touch-to-button navigation shim. On devices with a touchscreen but fewer
// than four physical buttons (e.g. Bangle.js 2), translate touch gestures into
// synthetic Pebble button events so the button-driven UI (launcher, menus,
// watchface) becomes navigable: swipe up/down -> UP/DOWN, tap -> SELECT. The
// synthetic events flow through the normal kernel button pipeline, so they are
// indistinguishable from hardware presses to every downstream ClickManager.
//
// This runs on the system task (the CST816 driver defers its work there), a
// non-ISR context, so event_put() is the correct queueing call.
static void prv_synthesize_nav_button(ButtonId button_id) {
#if CONFIG_TOUCH_NAV_BUTTONS
  PebbleEvent down = {
    .type = PEBBLE_BUTTON_DOWN_EVENT,
    .button.button_id = button_id,
  };
  PebbleEvent up = {
    .type = PEBBLE_BUTTON_UP_EVENT,
    .button.button_id = button_id,
  };
  PBL_LOG_DBG("Touch nav: synthesizing button %d (down+up)", button_id);
  event_put(&down);
  event_put(&up);
#else
  (void)button_id;
#endif
}

void touch_handle_gesture(TouchGesture gesture, int16_t x, int16_t y) {
  mutex_lock(s_touch_mutex);

  if (!s_globally_enabled) {
    mutex_unlock(s_touch_mutex);
    return;
  }

  prv_apply_rotation(&x, &y);

  PBL_LOG_DBG("Gesture: %d @ (%" PRId16 ", %" PRId16 ")", gesture, x, y);

  switch (gesture) {
    case TouchGesture_Tap:
      PBL_ANALYTICS_ADD(gesture_tap_count, 1);
      prv_put_gesture_event(GestureEvent_Tap, x, y);
      prv_synthesize_nav_button(BUTTON_ID_SELECT);
      break;
    case TouchGesture_DoubleTap:
      PBL_ANALYTICS_ADD(gesture_double_tap_count, 1);
      prv_put_gesture_event(GestureEvent_DoubleTap, x, y);
      break;
    case TouchGesture_SwipeUp:
      prv_synthesize_nav_button(BUTTON_ID_UP);
      break;
    case TouchGesture_SwipeDown:
      prv_synthesize_nav_button(BUTTON_ID_DOWN);
      break;
    default:
      break;
  }

  mutex_unlock(s_touch_mutex);
}

void touch_reset(void) {
  mutex_lock(s_touch_mutex);
  s_touch_state = TouchState_FingerUp;
  s_last_x = 0;
  s_last_y = 0;
  mutex_unlock(s_touch_mutex);
}

void touch_set_rotated(bool rotated) {
  mutex_lock(s_touch_mutex);
  s_rotated = rotated;
  mutex_unlock(s_touch_mutex);
}
