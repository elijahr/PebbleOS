/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "applib/ui/menu_layer.h"
#include "applib/ui/content_indicator_private.h"

#include "applib/ui/recognizer/drag.h"
#include "applib/ui/recognizer/recognizer.h"
#include "applib/ui/recognizer/recognizer_list.h"
#include "applib/ui/recognizer/recognizer_manager.h"
#include "applib/ui/recognizer/recognizer_private.h"
#include "applib/ui/window.h"
#include "services/touch/touch_click_suppress.h"

// Stubs
/////////////////////
#include "stubs_app_state.h"
#include "stubs_click.h"
#include "stubs_graphics.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_process_manager.h"
#include "stubs_resources.h"
#include "stubs_syscalls.h"
#include "stubs_tick.h"
#include "stubs_ui_window.h"
#include "stubs_unobstructed_area.h"
#include "stubs_vibes.h"

#include "fake_pebble_tasks.h"
#include "fake_rtc.h"

// Fakes
////////////////////////

GDrawState graphics_context_get_drawing_state(GContext *ctx) {
  return (GDrawState){};
}

void graphics_context_set_drawing_state(GContext *ctx, GDrawState draw_state) {}
void graphics_context_set_fill_color(GContext *ctx, GColor color) {}

Layer *inverter_layer_get_layer(InverterLayer *inverter_layer) {
  return &inverter_layer->layer;
}

void inverter_layer_init(InverterLayer *inverter, const GRect *frame) {}

void window_long_click_subscribe(ButtonId button_id, uint16_t delay_ms, ClickHandler down_handler,
                                 ClickHandler up_handler) {}
void window_single_click_subscribe(ButtonId button_id, ClickHandler handler) {}
void window_single_repeating_click_subscribe(ButtonId button_id, uint16_t repeat_interval_ms,
                                             ClickHandler handler) {}
void window_set_click_config_provider_with_context(Window *window,
                                                   ClickConfigProvider click_config_provider,
                                                   void *context) {}
void window_set_click_context(ButtonId button_id, void *context) {}

void content_indicator_destroy_for_scroll_layer(ScrollLayer *scroll_layer) {}

static ContentIndicator s_content_indicator;
ContentIndicator *content_indicator_get_for_scroll_layer(ScrollLayer *scroll_layer) {
  return &s_content_indicator;
}
ContentIndicator *content_indicator_get_or_create_for_scroll_layer(ScrollLayer *scroll_layer) {
  return &s_content_indicator;
}
void content_indicator_set_content_available(ContentIndicator *content_indicator,
                                             ContentIndicatorDirection direction, bool available) {}

void graphics_context_set_compositing_mode(GContext *ctx, GCompOp mode) {}
void graphics_draw_bitmap_in_rect(GContext *ctx, const GBitmap *bitmap, const GRect *rect) {}

int16_t menu_cell_basic_cell_height(void) {
  return 44;
}

// Symbol providers: window.c is NOT compiled in this suite (only its header is needed for
// RecognizerManager/RecognizerList types). Every test drives touch events directly on the
// MenuLayer's embedded ScrollLayer recognizer (recognizer_handle_touch_event), never through
// recognizer_manager_handle_touch_event, so these are link-time-only requirements of
// recognizer_manager.c -- same rationale as tests/fw/ui/test_scroll_layer_drag.c.
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

static RecognizerList s_app_recognizer_list;

RecognizerList *app_state_get_recognizer_list(void) {
  return &s_app_recognizer_list;
}

// Not declared in menu_layer.h; exercised directly, same as menu_up_click_handler and
// menu_down_click_handler are in test_menu_layer.c.
extern void menu_select_click_handler(ClickRecognizerRef recognizer, MenuLayer *menu_layer);

// Fetch the (single) recognizer the embedded ScrollLayer auto-attached, mirroring
// test_scroll_layer_drag.c's prv_drag_recognizer().
static Recognizer *prv_drag_recognizer(MenuLayer *menu_layer) {
  RecognizerList *list = layer_get_recognizer_list(&menu_layer->scroll_layer.layer);
  return (Recognizer *)list->node;
}

static void prv_send(MenuLayer *menu_layer, TouchEventType type, int16_t x, int16_t y) {
  recognizer_handle_touch_event(prv_drag_recognizer(menu_layer),
                                &(TouchEvent){.type = type, .x = x, .y = y});
}

// Drives the drag recognizer through touchdown + N position updates of dy=-20px each, landing
// the embedded ScrollLayer's content offset at 8 - 20*N (see drag.c: the first two updates cross
// DRAG_MIN_UPDATES/DRAG_START_THRESHOLD_PX and deliver cum_dy - threshold = -40 - (-8) = -32;
// every update after that delivers its own raw -20px step, unaccumulated).
static void prv_drag_updates(MenuLayer *menu_layer, int updates) {
  int16_t y = 300;
  prv_send(menu_layer, TouchEvent_Touchdown, 50, y);
  for (int i = 0; i < updates; i++) {
    y -= 20;
    prv_send(menu_layer, TouchEvent_PositionUpdate, 50, y);
  }
}

// Menu fixture
////////////////////////

static uint16_t s_num_rows;

static void prv_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                         void *callback_context) {}

static uint16_t prv_get_num_rows(struct MenuLayer *menu_layer, uint16_t section_index,
                                 void *callback_context) {
  return s_num_rows;
}

typedef struct SelectionChangedCall {
  MenuIndex new_index;
  MenuIndex old_index;
} SelectionChangedCall;

static SelectionChangedCall s_selection_changed_calls[4];
static int s_selection_changed_call_count;
static MenuIndex s_redirect_target;
static bool s_redirect_pending;

static void prv_selection_changed(struct MenuLayer *menu_layer, MenuIndex new_index,
                                  MenuIndex old_index, void *callback_context) {
  cl_assert(s_selection_changed_call_count < 4);
  s_selection_changed_calls[s_selection_changed_call_count] =
      (SelectionChangedCall){.new_index = new_index, .old_index = old_index};
  s_selection_changed_call_count++;

  if (s_redirect_pending) {
    s_redirect_pending = false;
    menu_layer_set_selected_index(menu_layer, s_redirect_target, MenuRowAlignTop, false);
  }
}

static MenuIndex s_select_click_index;
static int s_select_click_count;

static void prv_select_click(struct MenuLayer *menu_layer, MenuIndex *cell_index,
                             void *callback_context) {
  s_select_click_index = *cell_index;
  s_select_click_count++;
}

static MenuLayer s_menu_layer;

static void prv_init_menu(uint16_t num_rows, bool with_selection_changed) {
  s_num_rows = num_rows;
  s_selection_changed_call_count = 0;
  s_redirect_pending = false;
  s_select_click_count = 0;

  menu_layer_init(&s_menu_layer, &GRect(0, 0, 168, 168));
  menu_layer_pad_bottom_enable(&s_menu_layer, false);
  menu_layer_set_callbacks(
      &s_menu_layer, NULL,
      &(MenuLayerCallbacks){
          .draw_row = prv_draw_row,
          .get_num_rows = prv_get_num_rows,
          .select_click = prv_select_click,
          .selection_changed = with_selection_changed ? prv_selection_changed : NULL,
      });
}

// Setup
////////////////////////////////////

void test_menu_layer_drag__initialize(void) {
  recognizer_list_init(&s_app_recognizer_list);
  recognizer_manager_init(&s_manager);
  fake_rtc_init(1000 /* ticks */, 0 /* time */);
  stub_pebble_tasks_set_current(PebbleTask_App);
  *app_state_get_touch_click_suppress_state() = (TouchClickSuppressState){};
}

void test_menu_layer_drag__cleanup(void) {}

// Tests
////////////////////////////////////

// F-SM: content (2 rows * 44px = 88px) fits entirely inside the 168px frame, so the content
// offset can never actually change (scroll_layer's own clamp holds it at 0) -- the offset-changed
// handler (and therefore the reconciliation stub) never runs, per the real
// touch_click_suppress module (not a stub -- see the plan's green-mirage warning).
void test_menu_layer_drag__short_menu_never_fires_stub(void) {
  prv_init_menu(2, true);

  prv_drag_updates(&s_menu_layer, 2);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, 0);
  cl_assert_equal_i(0, s_menu_layer.selection.index.row);
  cl_assert_equal_i(0, s_selection_changed_call_count);

  prv_send(&s_menu_layer, TouchEvent_Liftoff, 50, 260);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}

// A small drag (offset -32) keeps row 0's [0,44] band overlapping the new visible band
// [32,200): selection must not move even though the offset did.
void test_menu_layer_drag__selection_kept_when_visible(void) {
  prv_init_menu(10, true);

  prv_drag_updates(&s_menu_layer, 2);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -32);
  cl_assert_equal_i(0, s_menu_layer.selection.index.row);
  cl_assert_equal_i(0, s_menu_layer.selection.y);
  cl_assert_equal_i(0, s_selection_changed_call_count);
}

// A larger drag (offset -52) pushes row 0's [0,44] band entirely above the new visible band
// [52,220): selection must move to row 1 ([44,88], overlapping), the offset must not snap, and
// the selection_changed callback must observe exactly the one expected transition.
void test_menu_layer_drag__nearest_visible_row_on_scrollout(void) {
  prv_init_menu(10, true);

  prv_drag_updates(&s_menu_layer, 3);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -52);
  cl_assert_equal_i(1, s_menu_layer.selection.index.row);
  cl_assert_equal_i(44, s_menu_layer.selection.y);

  cl_assert_equal_i(1, s_selection_changed_call_count);
  cl_assert_equal_i(0, s_selection_changed_calls[0].old_index.row);
  cl_assert_equal_i(1, s_selection_changed_calls[0].new_index.row);
}

// A selection_changed callback firing mid-reconciliation legally calls
// menu_layer_set_selected_index() with an off-screen row. The in_offset_reconcile guard must
// suppress the resulting scroll-position re-derivation (offset must stay at the reconcile-only
// value, -52), while the selection index itself must still update to the requested row.
void test_menu_layer_drag__reentry_guard_blocks_rederivation(void) {
  prv_init_menu(10, true);
  s_redirect_pending = true;
  s_redirect_target = MenuIndex(0, 9);

  prv_drag_updates(&s_menu_layer, 3);

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -52);
  cl_assert_equal_i(9, s_menu_layer.selection.index.row);

  cl_assert_equal_i(2, s_selection_changed_call_count);
  cl_assert_equal_i(0, s_selection_changed_calls[0].old_index.row);
  cl_assert_equal_i(1, s_selection_changed_calls[0].new_index.row);
  cl_assert_equal_i(1, s_selection_changed_calls[1].old_index.row);
  cl_assert_equal_i(9, s_selection_changed_calls[1].new_index.row);
}

// Ending a drag mid-cell (content_top_y = 52, strictly inside row 1's [44,88] band, not at a row
// boundary) and then invoking the select path must act on the reconciled selection (row 1), with
// no further offset movement.
void test_menu_layer_drag__select_after_mid_cell_drag(void) {
  prv_init_menu(10, true);

  prv_drag_updates(&s_menu_layer, 3);
  cl_assert_equal_i(1, s_menu_layer.selection.index.row);
  prv_send(&s_menu_layer, TouchEvent_Liftoff, 50, 240);

  menu_select_click_handler(NULL, &s_menu_layer);

  cl_assert_equal_i(1, s_select_click_count);
  cl_assert_equal_i(1, s_select_click_index.row);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -52);
}
