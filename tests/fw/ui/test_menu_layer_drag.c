/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "applib/ui/menu_layer.h"
#include "applib/ui/animation.h"
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
extern void menu_down_click_handler(ClickRecognizerRef recognizer, MenuLayer *menu_layer);

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

// Mirror image of prv_drag_updates(): finger moves DOWN (dy=+20px per update), dragging content
// downward and revealing earlier rows -- the walk_upward reconcile direction. By the same drag.c
// math (symmetric around 0), N updates add a cumulative delta of 20*N-8 to whatever offset the
// ScrollLayer already had.
static void prv_drag_updates_reverse(MenuLayer *menu_layer, int updates) {
  int16_t y = 100;
  prv_send(menu_layer, TouchEvent_Touchdown, 50, y);
  for (int i = 0; i < updates; i++) {
    y += 20;
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
static bool s_nested_offset_change_pending;

static void prv_selection_changed(struct MenuLayer *menu_layer, MenuIndex new_index,
                                  MenuIndex old_index, void *callback_context) {
  cl_assert(s_selection_changed_call_count < 4);
  s_selection_changed_calls[s_selection_changed_call_count] =
      (SelectionChangedCall){.new_index = new_index, .old_index = old_index};
  s_selection_changed_call_count++;

  if (s_nested_offset_change_pending) {
    s_nested_offset_change_pending = false;
    // Legal, public-SDK reentrancy: nudge the offset by 1px directly on the embedded ScrollLayer.
    // This re-enters prv_menu_scroll_offset_changed_handler synchronously, one frame deeper.
    ScrollLayer *scroll_layer = menu_layer_get_scroll_layer(menu_layer);
    const GPoint current = scroll_layer_get_content_offset(scroll_layer);
    scroll_layer_set_content_offset(scroll_layer, GPoint(current.x, current.y - 1), false);
  }

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
  s_nested_offset_change_pending = false;
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

// Content (2 rows * 44px = 88px) fits entirely inside the 168px frame, so scroll_layer's clamp
// pins the offset at 0 no matter how far the drag travels. The offset assertion is therefore a
// FIXTURE PRECONDITION, not a result: it documents that this menu really is unscrollable.
//
// The click assertion is the one under test, and it is falsifiable. Suppression hangs on
// scroll_layer.c's `if (after.y != before.y)` guard around touch_click_suppress_mark_consumed():
// drop that guard so a drag marks the click consumed unconditionally, and this test fails
// (should_drop_click() returns true). It is the only case in this suite that catches that
// mutation -- every other drag test scrolls a long menu, where the guard is true either way.
// Concretely: a user flicking a two-item menu must still get a select-click.
void test_menu_layer_drag__short_menu_does_not_suppress_click(void) {
  prv_init_menu(2, true);

  prv_drag_updates(&s_menu_layer, 2);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, 0);

  prv_send(&s_menu_layer, TouchEvent_Liftoff, 50, 260);
  cl_assert_equal_b(touch_click_suppress_should_drop_click(), false);
}

// Seeds a NON-DEFAULT selection (row 3) before dragging: asserting row==0/y==0 after a drag (the
// default, untouched state) would be indistinguishable from the reconciliation handler never
// running at all. Seeding row 3 first, then confirming it survives, proves the handler actually
// evaluated the still-visible case and chose not to move it.
void test_menu_layer_drag__selection_kept_when_visible(void) {
  prv_init_menu(10, true);

  menu_layer_set_selected_index(&s_menu_layer, MenuIndex(0, 3), MenuRowAlignNone, false);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, 0);
  cl_assert_equal_i(3, s_menu_layer.selection.index.row);
  cl_assert_equal_i(132, s_menu_layer.selection.y);
  s_selection_changed_call_count = 0;  // discard the set_selected_index announcement

  // Offset -32 keeps row 3's [132,176] band overlapping the new visible band [32,200).
  prv_drag_updates(&s_menu_layer, 2);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -32);
  cl_assert_equal_i(3, s_menu_layer.selection.index.row);
  cl_assert_equal_i(132, s_menu_layer.selection.y);
  cl_assert_equal_i(0, s_selection_changed_call_count);
}

// Boundary pair for the visibility check in prv_menu_scroll_offset_changed_handler
// (selection_bottom > content_top_y): row 0's band is [0,44]. One pixel short of scrolling it
// out (offset -43, content_top_y=43) must KEEP the selection.
void test_menu_layer_drag__selection_kept_one_px_before_scrollout(void) {
  prv_init_menu(10, true);

  int16_t y = 300;
  prv_send(&s_menu_layer, TouchEvent_Touchdown, 50, y);
  y -= 20;
  prv_send(&s_menu_layer, TouchEvent_PositionUpdate, 50, y);
  y -= 20;
  prv_send(&s_menu_layer, TouchEvent_PositionUpdate, 50, y);  // offset -32, recognizer Started
  y -= 11;
  prv_send(&s_menu_layer, TouchEvent_PositionUpdate, 50, y);  // offset -43

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -43);
  cl_assert_equal_i(0, s_menu_layer.selection.index.row);
  cl_assert_equal_i(0, s_menu_layer.selection.y);
  cl_assert_equal_i(0, s_selection_changed_call_count);
}

// The other half of the boundary pair: one pixel further (offset -44, content_top_y=44) makes
// row 0's band [0,44] no longer overlap ([44,44] touches but does not overlap per the
// `>`/strict-overlap check), so the selection must MOVE to row 1 ([44,88]).
void test_menu_layer_drag__selection_moves_at_scrollout_threshold(void) {
  prv_init_menu(10, true);

  int16_t y = 300;
  prv_send(&s_menu_layer, TouchEvent_Touchdown, 50, y);
  y -= 20;
  prv_send(&s_menu_layer, TouchEvent_PositionUpdate, 50, y);
  y -= 20;
  prv_send(&s_menu_layer, TouchEvent_PositionUpdate, 50, y);  // offset -32, recognizer Started
  y -= 12;
  prv_send(&s_menu_layer, TouchEvent_PositionUpdate, 50, y);  // offset -44

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -44);
  cl_assert_equal_i(1, s_menu_layer.selection.index.row);
  cl_assert_equal_i(44, s_menu_layer.selection.y);
  cl_assert_equal_i(1, s_selection_changed_call_count);
  cl_assert_equal_i(0, s_selection_changed_calls[0].old_index.row);
  cl_assert_equal_i(1, s_selection_changed_calls[0].new_index.row);
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

// The highlight (inverter) layer must reposition to track the reconciled selection -- otherwise
// the highlighted row and the row that activates on a post-drag tap diverge. Same 3-update drag
// as nearest_visible_row_on_scrollout: selection moves from row 0 (y=0) to row 1 (y=44,h=44).
void test_menu_layer_drag__highlight_follows_reconciled_selection(void) {
  prv_init_menu(10, true);

  prv_drag_updates(&s_menu_layer, 3);

  cl_assert_equal_i(1, s_menu_layer.selection.index.row);
  cl_assert_equal_i(44, s_menu_layer.selection.y);

  cl_assert_equal_i(0, s_menu_layer.inverter.layer.frame.origin.x);
  cl_assert_equal_i(44, s_menu_layer.inverter.layer.frame.origin.y);
  cl_assert_equal_i(168, s_menu_layer.inverter.layer.frame.size.w);
  cl_assert_equal_i(44, s_menu_layer.inverter.layer.frame.size.h);
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

// The reconcile logic must be scoped to an actual finger drag, not to "any content offset
// change". A plain, non-animated jump on the ScrollLayer (the kind menu_layer_reload_data() or a
// direct scroll_layer_set_content_offset() call would produce) moves the offset exactly like a
// drag does, but must not reconcile the selection: only prv_drag_event_cb's synchronous call is a
// real drag. Selection sits at row 50 (y=2200); the jump makes rows around y=[100,268) visible,
// nowhere near row 50 -- if reconciliation ran, it would move the selection to the nearest
// visible row (row 6). It must not.
void test_menu_layer_drag__programmatic_offset_change_does_not_reconcile(void) {
  prv_init_menu(60, true);
  menu_layer_set_selected_index(&s_menu_layer, MenuIndex(0, 50), MenuRowAlignNone, false);
  cl_assert_equal_i(50, s_menu_layer.selection.index.row);
  cl_assert_equal_i(2200, s_menu_layer.selection.y);
  s_selection_changed_call_count = 0;

  scroll_layer_set_content_offset(&s_menu_layer.scroll_layer, GPoint(0, -100), false);

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -100);
  cl_assert_equal_i(50, s_menu_layer.selection.index.row);
  cl_assert_equal_i(2200, s_menu_layer.selection.y);
  cl_assert_equal_i(0, s_selection_changed_call_count);
}

// Self-contained variant of the test above: the original only catches a missing
// `s_dragging_scroll_layer = NULL` clear (scroll_layer.c's prv_drag_event_cb) when some OTHER
// case that drives a real drag happens to run first in the same clar process and leaves that
// file-static pointer pointed at this suite's (address-stable) s_menu_layer.scroll_layer --
// order-dependent, and invisible when this case runs alone (`runme -tprogrammatic_...`). This
// case drives a real drag to completion itself first, so the set/clear is exercised and checked
// within a single test body regardless of run order or filtering.
void test_menu_layer_drag__programmatic_offset_change_after_drag_does_not_reconcile(void) {
  prv_init_menu(60, true);

  // Real drag to completion (offset -52, see nearest_visible_row_on_scrollout): exercises
  // prv_drag_event_cb's set of s_dragging_scroll_layer during the drag and, if the clear at the
  // end of that function is intact, leaves it NULL again once this call returns.
  prv_drag_updates(&s_menu_layer, 3);
  prv_send(&s_menu_layer, TouchEvent_Liftoff, 50, 220);
  cl_assert_equal_i(1, s_menu_layer.selection.index.row);

  menu_layer_set_selected_index(&s_menu_layer, MenuIndex(0, 50), MenuRowAlignNone, false);
  cl_assert_equal_i(50, s_menu_layer.selection.index.row);
  cl_assert_equal_i(2200, s_menu_layer.selection.y);
  s_selection_changed_call_count = 0;

  // If s_dragging_scroll_layer were left pointing at this scroll_layer (missing clear), this
  // programmatic call would be misidentified as a drag and incorrectly reconcile row 50 away.
  scroll_layer_set_content_offset(&s_menu_layer.scroll_layer, GPoint(0, -100), false);

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -100);
  cl_assert_equal_i(50, s_menu_layer.selection.index.row);
  cl_assert_equal_i(2200, s_menu_layer.selection.y);
  cl_assert_equal_i(0, s_selection_changed_call_count);
}

// A stale, still-scheduled selection (inverter) animation left over from a synthesized button
// click (bangle2 runs with CONFIG_TOUCH_NAV_BUTTONS=1) must not survive a drag reconciliation:
// its next update would otherwise overwrite the reconciled highlight with a stale target. Row 0
// -> row 1 via menu_down_click_handler() schedules such an animation (never ticked -- the linked
// stubs_animation.c only flips a "scheduled" flag); a drag then pushes row 1 out of view, which
// must reconcile the selection AND cancel the stale animation.
void test_menu_layer_drag__drag_reconcile_cancels_stale_selection_animation(void) {
  prv_init_menu(10, true);

  menu_down_click_handler(NULL, &s_menu_layer);
  cl_assert_equal_i(1, s_menu_layer.selection.index.row);
  cl_assert(s_menu_layer.animation.animation != NULL);
  cl_assert_equal_b(true, animation_is_scheduled(s_menu_layer.animation.animation));

  // 6 updates of dy=-20 land the offset at 8-20*6=-112 (see prv_drag_updates), pushing row 1's
  // [44,88] band out of the new [112,280) visible band; reconciliation walks downward to row 2
  // ([88,132], overlapping).
  prv_drag_updates(&s_menu_layer, 6);

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -112);
  cl_assert_equal_i(2, s_menu_layer.selection.index.row);
  cl_assert_equal_i(88, s_menu_layer.selection.y);
  cl_assert(s_menu_layer.animation.animation == NULL);
}

// A center-focus selection animation stages menu_layer->selection at the *previous* index while
// it animates toward animation.new_selection (see prv_schedule_center_focus_animation): a drag
// reconciling mid-animation would clobber that staged state and, on the animation's next update,
// get silently overwritten again (also snapping the content offset out from under the drag). The
// reconcile handler must bail out entirely while such an animation is in flight, leaving both the
// staged selection and the scheduled animation untouched; the drag itself (offset) still applies.
void test_menu_layer_drag__center_focus_animation_in_flight_blocks_reconcile(void) {
  prv_init_menu(10, true);
  menu_layer_set_center_focused(&s_menu_layer, true);
  cl_assert_equal_b(true, menu_layer_get_center_focused(&s_menu_layer));

  // Advance selection with animation: schedules a center-focus animation and stages
  // menu_layer->selection back to the pre-click (row 0) index while animation.new_selection holds
  // the real target (row 1).
  menu_down_click_handler(NULL, &s_menu_layer);
  cl_assert_equal_i(0, s_menu_layer.selection.index.row);
  cl_assert_equal_i(1, s_menu_layer.animation.new_selection.index.row);
  cl_assert(s_menu_layer.animation.animation != NULL);
  cl_assert_equal_b(true, animation_is_scheduled(s_menu_layer.animation.animation));

  // prv_schedule_center_focus_animation() already snapped the offset once, unanimated, to center
  // the (staged, row 0) selection: y = 168/2 - 0 - 44/2 = 62. The drag then applies its own delta
  // on top: 6 updates of dy=-20 contribute 8-20*6=-112 (see prv_drag_updates), landing at
  // 62 + (-112) = -50.
  prv_drag_updates(&s_menu_layer, 6);

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -50);
  // Reconciliation must not have run: staged selection and in-flight animation untouched.
  cl_assert_equal_i(0, s_menu_layer.selection.index.row);
  cl_assert_equal_i(1, s_menu_layer.animation.new_selection.index.row);
  cl_assert(s_menu_layer.animation.animation != NULL);
  cl_assert_equal_b(true, animation_is_scheduled(s_menu_layer.animation.animation));
}

// A selection_changed callback firing mid-reconciliation may legally call
// scroll_layer_set_content_offset() directly (public SDK) before also calling
// menu_layer_set_selected_index() -- both in the same callback invocation. The nested offset call
// re-enters prv_menu_scroll_offset_changed_handler, and that inner frame's exit must not clear the
// OUTER frame's in_offset_reconcile guard (must save/restore, not set-true/clear-false): otherwise
// the subsequent menu_layer_set_selected_index() call sees the guard down and wrongly re-derives
// the content offset from the new selection, clobbering what the drag (plus the nested nudge) set.
void test_menu_layer_drag__reentrant_offset_change_preserves_reconcile_guard(void) {
  // 30 rows (1320px of content) so the redirect target's top-aligned offset (-396, see below)
  // isn't clamped by the ScrollLayer's own max-scroll clipping (which a 10-row/440px menu would
  // hit at -272) -- that would mask the bug this test targets.
  prv_init_menu(30, true);
  s_nested_offset_change_pending = true;
  s_redirect_pending = true;
  s_redirect_target = MenuIndex(0, 9);

  prv_drag_updates(&s_menu_layer, 3);

  cl_assert_equal_i(9, s_menu_layer.selection.index.row);
  // Drag reconciliation lands the offset at -52 (row 0 -> row 1, see
  // nearest_visible_row_on_scrollout); the nested call nudges it to -53. The
  // menu_layer_set_selected_index(..., MenuRowAlignTop) call from the same callback must NOT be
  // allowed to re-derive it to -(9*44) = -396.
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -53);

  cl_assert_equal_i(2, s_selection_changed_call_count);
  cl_assert_equal_i(0, s_selection_changed_calls[0].old_index.row);
  cl_assert_equal_i(1, s_selection_changed_calls[0].new_index.row);
  cl_assert_equal_i(1, s_selection_changed_calls[1].old_index.row);
  cl_assert_equal_i(9, s_selection_changed_calls[1].new_index.row);
}

// Covers the walk_upward reconcile direction (prv_menu_layer_walk_upward_from_iterator), which
// every other case in this file leaves untested: they all drag content upward (dy negative),
// pushing the selection ABOVE the viewport and taking the walk_downward branch. Here the drag
// goes the other way: content moves down, revealing earlier rows, and a selection further down
// the menu ends up BELOW the viewport, taking the walk_upward branch instead.
void test_menu_layer_drag__nearest_visible_row_below_viewport_walks_upward(void) {
  prv_init_menu(10, true);
  // Put row 5 (y=220, h=44) at the very top of the frame first (non-drag jump; harmless per
  // programmatic_offset_change_does_not_reconcile).
  menu_layer_set_selected_index(&s_menu_layer, MenuIndex(0, 5), MenuRowAlignTop, false);
  cl_assert_equal_i(5, s_menu_layer.selection.index.row);
  cl_assert_equal_i(220, s_menu_layer.selection.y);
  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -220);
  s_selection_changed_call_count = 0;

  // 9 updates of dy=+20 add a cumulative 20*9-8=172 to the existing -220 offset, landing at -48
  // (content band [48,216)). Row 5's [220,264) band no longer overlaps: it's entirely below the
  // viewport (220 >= 216), so reconciliation must walk upward from row 5 and land on row 4
  // ([176,220), which does overlap: 220>48 and 176<216).
  prv_drag_updates_reverse(&s_menu_layer, 9);

  cl_assert_equal_i(scroll_layer_get_content_offset(&s_menu_layer.scroll_layer).y, -48);
  cl_assert_equal_i(4, s_menu_layer.selection.index.row);
  cl_assert_equal_i(176, s_menu_layer.selection.y);

  cl_assert_equal_i(1, s_selection_changed_call_count);
  cl_assert_equal_i(5, s_selection_changed_calls[0].old_index.row);
  cl_assert_equal_i(4, s_selection_changed_calls[0].new_index.row);
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
