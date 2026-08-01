/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "app_window_recognizer_glue.h"

#ifdef CONFIG_TOUCH

#include "applib/event_service_client.h"
#include "applib/touch_service.h"
#include "applib/touch_service_private.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/recognizer/recognizer_manager.h"
#include "applib/ui/window.h"
#include "kernel/events.h"
#include "process_state/app_state/app_state.h"
#include "services/touch/touch_click_suppress.h"
#include "pbl/services/touch/touch_event.h"
#include "pbl/util/attributes.h"

#include <stddef.h>

// Task 8 (impl plan section 12): the single touch_service raw handler for the app task.
// Suppression note comes first so a click synthesized off this same stroke can be dropped by
// the widget's mark_consumed call before the click ever reaches dispatch (Task 7).
static void prv_touch_handler(const TouchEvent *event, void *context) {
  touch_click_suppress_note_touch_event(event);
  recognizer_manager_handle_touch_event(event, app_state_get_recognizer_manager());
}

// Focus-event handler: mirrors app_focus_service's PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT plumbing.
// T_STATIC so tests can drive it directly without a full event-service subscription.
T_STATIC void prv_focus_event_handler(PebbleEvent *e, void *context) {
  RecognizerManager *mgr = app_state_get_recognizer_manager();
  bool in_focus = e->app_focus.in_focus;

  if (!in_focus) {
    // Defense in depth: order vs prv_app_will_focus_handler (app.c) is undefined; the NULL/
    // window guard makes either order correct.
    if (mgr && mgr->window) {
      recognizer_manager_cancel_touches(mgr);
    }
    return;
  }

  if (!mgr) {
    return;
  }
  // Regain path (app.c's window_set_on_screen(window, true, false)) does not reach the
  // appear seam because is_waiting_for_click_config was already cleared -- repoint and reset
  // here instead.
  recognizer_manager_set_window(mgr, app_window_stack_get_top_window());
  recognizer_manager_reset(mgr);
}

void app_window_recognizer_glue_attach_count_changed(uint16_t new_count) {
  AppWindowRecognizerGlueState *state = app_state_get_recognizer_glue_state();

  // Edge-triggered on state, not on new_count's value: new_count == 1 is reachable from both
  // a genuine 0->1 attach and a 2->1 detach (e.g. an app with two ScrollLayers destroying
  // one), and only the former should (re-)subscribe.
  if ((new_count == 1) && !state->touch_subscribed) {
    touch_service_subscribe(prv_touch_handler, NULL);
    state->touch_subscribed = true;
    if (!state->focus_subscribed) {
      state->focus_event_info = (EventServiceInfo) {
        .type = PEBBLE_APP_WILL_CHANGE_FOCUS_EVENT,
        .handler = prv_focus_event_handler,
      };
      event_service_client_subscribe(&state->focus_event_info);
      state->focus_subscribed = true;
    }
  } else if ((new_count == 0) && state->touch_subscribed) {
    // Only unsubscribe if our handler is still installed: an app that called
    // touch_service_subscribe() after us now owns the (single) raw_handler slot, and
    // unsubscribing here would silently clear the app's handler instead of ours.
    TouchServiceState *touch_state = app_state_get_touch_service_state();
    if (touch_state && (touch_state->raw_handler == prv_touch_handler)) {
      touch_service_unsubscribe();
    }
    state->touch_subscribed = false;
    if (state->focus_subscribed) {
      event_service_client_unsubscribe(&state->focus_event_info);
      state->focus_subscribed = false;
    }
  }
}

void app_window_recognizer_glue_window_focused(struct Window *window) {
  RecognizerManager *mgr = window_get_recognizer_manager(window);
  if (!mgr) {
    return;
  }
  // Set-before-cancel (design Section 6 point 5): the cancel walk must see the new window.
  recognizer_manager_set_window(mgr, window);
  recognizer_manager_cancel_touches(mgr);
}

void app_window_recognizer_glue_window_off_screen(struct Window *window) {
  RecognizerManager *mgr = window_get_recognizer_manager(window);
  if (!mgr || (mgr->window != window)) {
    return;
  }
  recognizer_manager_cancel_touches(mgr);
  recognizer_manager_reset(mgr);
  recognizer_manager_set_window(mgr, NULL);
}

#endif // CONFIG_TOUCH
