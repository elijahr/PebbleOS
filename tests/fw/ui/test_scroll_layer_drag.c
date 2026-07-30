/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/ui/scroll_layer.h"

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
// window_get_recognizer_list/window_get_root_layer are link-time requirements
// of recognizer_manager.c (used only on the manager's touch-dispatch path,
// which this suite never drives -- recognizer_handle_touch_event is called
// directly on the recognizer instead) and are never exercised at runtime.
static RecognizerManager s_manager;

RecognizerManager *window_get_recognizer_manager(Window *window) {
  return &s_manager;
}

RecognizerList *window_get_recognizer_list(Window *window) {
  return NULL;
}

Layer *window_get_root_layer(const Window *window) {
  return NULL;
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
  scroll_layer_set_content_size(&scroll_layer, GSize(168, 400));

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

  // x is never modified
  cl_assert_equal_i(scroll_layer_get_content_offset(&scroll_layer).x, 0);

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
