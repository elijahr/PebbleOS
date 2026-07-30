/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/ui/scroll_layer.h"

#include "applib/ui/recognizer/drag.h"
#include "applib/ui/recognizer/recognizer.h"
#include "applib/ui/recognizer/recognizer_list.h"
#include "applib/ui/recognizer/recognizer_manager.h"
#include "applib/ui/recognizer/recognizer_private.h"
#include "applib/ui/window.h"
#include "services/touch/touch_click_suppress.h"

#include "clar.h"

// Stubs
////////////////////////////////////
#include "stubs_app_state.h"
#include "stubs_compiled_with_legacy2_sdk.h"
#include "stubs_content_indicator.h"
#include "stubs_graphics_context.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_resources.h"
#include "stubs_syscalls.h"
#include "stubs_tick.h"
#include "stubs_unobstructed_area.h"

#include "fake_pebble_tasks.h"
#include "fake_rtc.h"

static GRect s_graphics_draw_bitmap_in_rect__rect = GRectZero;

void graphics_draw_bitmap_in_rect(GContext *ctx, const GBitmap *src_bitmap, const GRect *rect) {
  s_graphics_draw_bitmap_in_rect__rect = *rect;
}
bool graphics_release_frame_buffer(GContext *ctx, GBitmap *buffer) {
  return false;
}
void window_schedule_render(struct Window *window) {}
void window_set_click_config_provider_with_context(struct Window *window,
                                                   ClickConfigProvider click_config_provider,
                                                   void *context) {}
void window_set_click_context(ButtonId button_id, void *context) {}
void window_single_repeating_click_subscribe(ButtonId button_id, uint16_t repeat_interval_ms,
                                             ClickHandler handler) {}

// Symbol providers: window.c is NOT compiled in this suite (only its header is
// needed for RecognizerManager/RecognizerList types). layer.c calls these
// through window_get_window(layer) -- window is always NULL for a standalone
// ScrollLayer, so window_get_recognizer_manager ignores its argument and
// always returns the suite-owned manager (Section 2.3 local-fixture pattern).
//
// window_get_recognizer_list/window_get_root_layer used to always return
// NULL and were link-time-only requirements of recognizer_manager.c, never
// exercised at runtime, because every test in this suite called
// recognizer_handle_touch_event directly on the recognizer instead of going
// through the manager. The manager-dispatch test below
// (manager_dispatch_moves_offset) needs the real path, so both now
// dereference a real Window when one is supplied, mirroring window.c's
// actual window_get_root_layer()/window_get_recognizer_list()
// (window.c itself remains uncompiled here; only this minimal slice of its
// behavior is reproduced).
static RecognizerManager s_manager;

RecognizerManager *window_get_recognizer_manager(Window *window) {
  return &s_manager;
}

Layer *window_get_root_layer(const Window *window) {
  return window ? (Layer *)&window->layer : NULL;
}

RecognizerList *window_get_recognizer_list(Window *window) {
  return window ? layer_get_recognizer_list(window_get_root_layer(window)) : NULL;
}

// app_state_get_recognizer_list is deliberately absent from stubs_app_state.h
// (Section 2.3 rule); recognizer_manager.c's dispatch path reads it
// unconditionally, so every suite that links recognizer_manager.c without
// window.c must supply its own fixture, same as test_recognizer_manager.c.
static RecognizerList s_app_recognizer_list;

RecognizerList *app_state_get_recognizer_list(void) {
  return &s_app_recognizer_list;
}

// Fetch the (single) recognizer the ScrollLayer auto-attached, mirroring the
// cast recognizer_list_iterate itself performs (RecognizerList.node is the
// list node embedded as the first member of struct Recognizer).
static Recognizer *prv_drag_recognizer(ScrollLayer *scroll_layer) {
  RecognizerList *list = layer_get_recognizer_list(scroll_layer_get_layer(scroll_layer));
  return (Recognizer *)list->node;
}

static void prv_send(ScrollLayer *scroll_layer, TouchEventType type, int16_t x, int16_t y) {
  recognizer_handle_touch_event(prv_drag_recognizer(scroll_layer),
                                &(TouchEvent){.type = type, .x = x, .y = y});
}

// Setup
////////////////////////////////////

void test_scroll_layer_drag__initialize(void) {
  recognizer_list_init(&s_app_recognizer_list);
  // Reset the suite-owned manager (including ->window, which
  // manager_dispatch_moves_offset points at a stack-local Window) so no test
  // can observe another test's manager state or a dangling window pointer.
  recognizer_manager_init(&s_manager);
  fake_rtc_init(1000 /* ticks */, 0 /* time */);
  stub_pebble_tasks_set_current(PebbleTask_App);
  *app_state_get_touch_click_suppress_state() = (TouchClickSuppressState){};
}

void test_scroll_layer_drag__cleanup(void) {}

// tests
////////////////////////////////////

void test_scroll_layer_drag__attach_on_init(void) {
  ScrollLayer scroll_layer;
  scroll_layer_init(&scroll_layer, &GRect(0, 0, 168, 168));

  RecognizerList *list = layer_get_recognizer_list(&scroll_layer.layer);
  cl_assert(list->node != NULL);

  scroll_layer_deinit(&scroll_layer);
}

// Drags the content offset by the delivered per-event dy, and clamps at the
// scroll_layer choke point once travel exceeds the content bounds.
void test_scroll_layer_drag__drag_moves_offset_with_clamp(void) {
  ScrollLayer scroll_layer;
  scroll_layer_init(&scroll_layer, &GRect(0, 0, 168, 168));
  // Content wider than the frame (200 vs 168) so a nonzero x offset has room
  // to exist without being clamped straight back to 0
  scroll_layer_set_content_size(&scroll_layer, GSize(200, 400));

  // Nonzero starting x: proves the vertical-only contract actually holds,
  // rather than merely being consistent with an untouched x == 0
  scroll_layer_set_content_offset(&scroll_layer, GPoint(-7, 0), false);
  cl_assert_equal_i(scroll_layer_get_content_offset(&scroll_layer).x, -7);

  // Touchdown, then two updates dy=-20 each (finger drags up): cum_dy=-40,
  // updates=2 -> Started, delivered = -40 - (-8) = -32
  prv_send(&scroll_layer, TouchEvent_Touchdown, 50, 300);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 280);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 260);
  cl_assert_equal_i(scroll_layer_get_content_offset(&scroll_layer).y, -32);

  // Third update dy=-260 (well past the -232 clamp: 168 - 400): offset must
  // clamp, not track the raw delta
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 0);
  cl_assert_equal_i(scroll_layer_get_content_offset(&scroll_layer).y, -232);

  // x is never modified by the drag
  cl_assert_equal_i(scroll_layer_get_content_offset(&scroll_layer).x, -7);

  prv_send(&scroll_layer, TouchEvent_Liftoff, 50, 0);
  scroll_layer_deinit(&scroll_layer);
}

// F-SM precondition: content fits entirely inside the frame, so the offset
// never actually changes -- the mark must never fire (real suppression state
// machine, per the plan's green-mirage warning).
void test_scroll_layer_drag__clamped_drag_not_consumed(void) {
  ScrollLayer scroll_layer;
  scroll_layer_init(&scroll_layer, &GRect(0, 0, 168, 168));
  scroll_layer_set_content_size(&scroll_layer, GSize(168, 100));

  prv_send(&scroll_layer, TouchEvent_Touchdown, 50, 100);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 80);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 60);
  cl_assert_equal_i(scroll_layer_get_content_offset(&scroll_layer).y, 0);

  prv_send(&scroll_layer, TouchEvent_Liftoff, 50, 60);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);

  scroll_layer_deinit(&scroll_layer);
}

// A drag that actually moves the offset must mark the stroke consumed so the
// synthetic click for the same stroke is dropped downstream.
void test_scroll_layer_drag__consumed_on_real_change(void) {
  ScrollLayer scroll_layer;
  scroll_layer_init(&scroll_layer, &GRect(0, 0, 168, 168));
  scroll_layer_set_content_size(&scroll_layer, GSize(168, 400));

  prv_send(&scroll_layer, TouchEvent_Touchdown, 50, 300);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 280);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 260);
  cl_assert(scroll_layer_get_content_offset(&scroll_layer).y != 0);

  prv_send(&scroll_layer, TouchEvent_Liftoff, 50, 260);
  // should_drop_click clears on read: assert true once, then false
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), true);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);

  scroll_layer_deinit(&scroll_layer);
}

// Pins the "no-op on cancel" contract: a real drag left in Updated (the
// steady state of an in-progress drag) and then cancelled must not move the
// offset further, and drag.c's prv_cancel must zero event_dy.
//
// Cancel entry point chosen: recognizer_cancel() directly, not
// recognizer_manager_cancel_touches()/recognizer_reset(). The real glue
// (app_window_recognizer_glue.c) calls recognizer_manager_cancel_touches(),
// but that walks recognizer lists reachable from the manager's window/active
// layer (recognizer_manager.c prv_process_all_recognizers) -- scaffolding
// this suite intentionally does not stand up (see the window_get_* stub
// comments above; that path is exercised by the dispatch test below
// instead). recognizer_manager_cancel_touches()'s own per-recognizer
// termination step, prv_cancel_or_fail_recognizer(), calls exactly
// recognizer_cancel() for any recognizer not in RecognizerState_Possible
// (recognizer_manager.c ~:154-159) -- i.e. exactly Started/Updated, which is
// what this test drives the recognizer into. Calling recognizer_cancel()
// directly exercises that identical terminal call without duplicating the
// manager/window/layer-tree fixture that manager_dispatch_moves_offset below
// builds for its own purpose.
void test_scroll_layer_drag__updated_drag_cancel_is_noop(void) {
  ScrollLayer scroll_layer;
  scroll_layer_init(&scroll_layer, &GRect(0, 0, 168, 168));
  scroll_layer_set_content_size(&scroll_layer, GSize(168, 400));

  // Touchdown + 3 updates: crosses the start threshold (Possible -> Started)
  // then advances to Updated, the steady state of a real in-progress drag.
  prv_send(&scroll_layer, TouchEvent_Touchdown, 50, 300);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 280);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 260);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 230);

  Recognizer *recognizer = prv_drag_recognizer(&scroll_layer);
  cl_assert_equal_i(recognizer_get_state(recognizer), RecognizerState_Updated);

  const GPoint offset_before_cancel = scroll_layer_get_content_offset(&scroll_layer);
  cl_assert(offset_before_cancel.y != 0);

  recognizer_cancel(recognizer);

  cl_assert_equal_i(recognizer_get_state(recognizer), RecognizerState_Cancelled);
  // No further movement: the Cancelled arm in scroll_layer.c's event
  // callback is a no-op
  const GPoint offset_after_cancel = scroll_layer_get_content_offset(&scroll_layer);
  cl_assert_equal_i(offset_after_cancel.x, offset_before_cancel.x);
  cl_assert_equal_i(offset_after_cancel.y, offset_before_cancel.y);
  // drag.c's prv_cancel zeroes event_dy
  cl_assert_equal_i(drag_recognizer_get_delta_y(recognizer), 0);

  scroll_layer_deinit(&scroll_layer);
}

// Drives touch events through the real manager dispatch path
// (recognizer_manager_handle_touch_event) instead of calling
// recognizer_handle_touch_event directly on the recognizer as prv_send()
// does everywhere else in this suite. This is the path a real touch driver
// actually uses (via the touch service's handler registration), and it
// exercises three things the direct-call tests above cannot: (a) active-layer
// hit-testing resolving a touch inside the ScrollLayer's frame to a
// descendant layer whose parent walk reaches the ScrollLayer's own
// recognizer list (recognizer_manager.c prv_process_all_recognizers); (b)
// layer_attach_recognizer having actually put the recognizer on that list, so
// the walk in (a) finds it; (c) the manager->triggered / fail / reset
// bookkeeping recognizer_manager.c performs around dispatch.
void test_scroll_layer_drag__manager_dispatch_moves_offset(void) {
  Window window = {};
  layer_init(&window.layer, &GRect(0, 0, 168, 168));
  window.layer.window = &window;

  ScrollLayer scroll_layer;
  scroll_layer_init(&scroll_layer, &GRect(0, 0, 168, 168));
  scroll_layer_set_content_size(&scroll_layer, GSize(168, 400));
  layer_add_child(&window.layer, &scroll_layer.layer);

  recognizer_manager_set_window(&s_manager, &window);

  // Touchdown, then two updates dy=-20 each: same math as
  // drag_moves_offset_with_clamp, but every event now goes through
  // recognizer_manager_handle_touch_event and must land inside the window's
  // 168x168 frame for hit-testing to resolve an active layer at all.
  recognizer_manager_handle_touch_event(
      &(TouchEvent){.type = TouchEvent_Touchdown, .x = 50, .y = 100}, &s_manager);
  cl_assert(s_manager.active_layer != NULL);

  recognizer_manager_handle_touch_event(
      &(TouchEvent){.type = TouchEvent_PositionUpdate, .x = 50, .y = 80}, &s_manager);
  recognizer_manager_handle_touch_event(
      &(TouchEvent){.type = TouchEvent_PositionUpdate, .x = 50, .y = 60}, &s_manager);

  cl_assert_equal_i(scroll_layer_get_content_offset(&scroll_layer).y, -32);
  cl_assert(s_manager.triggered != NULL);

  recognizer_manager_handle_touch_event(&(TouchEvent){.type = TouchEvent_Liftoff, .x = 50, .y = 60},
                                        &s_manager);

  scroll_layer_deinit(&scroll_layer);
}

void test_scroll_layer_drag__deinit_reclaims_recognizer(void) {
  ScrollLayer scroll_layer;
  scroll_layer_init(&scroll_layer, &GRect(0, 0, 168, 168));
  scroll_layer_set_content_size(&scroll_layer, GSize(168, 400));

  prv_send(&scroll_layer, TouchEvent_Touchdown, 50, 300);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 280);
  prv_send(&scroll_layer, TouchEvent_PositionUpdate, 50, 260);
  prv_send(&scroll_layer, TouchEvent_Liftoff, 50, 260);

  scroll_layer_deinit(&scroll_layer);

  cl_assert(layer_get_recognizer_list(&scroll_layer.layer)->node == NULL);
}
