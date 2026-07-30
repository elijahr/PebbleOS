/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "applib/touch_service.h"
#include "applib/ui/app_window_recognizer_glue.h"
#include "applib/ui/layer.h"
#include "applib/ui/window.h"
#include "applib/ui/window_stack_private.h"
#include "applib/ui/recognizer/recognizer.h"
#include "applib/ui/recognizer/recognizer_impl.h"
#include "applib/ui/recognizer/recognizer_list.h"
#include "applib/ui/recognizer/recognizer_manager.h"
#include "applib/ui/recognizer/recognizer_private.h"
#include "pbl/util/size.h"


// Stubs
#include "stubs_app_install_manager.h"
#include "stubs_app_state.h"
#include "stubs_event_service_client.h"
#include "stubs_gbitmap.h"
#include "stubs_graphics.h"
#include "stubs_graphics_context.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_new_timer.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_powermode_service.h"
#include "stubs_process_manager.h"
#include "stubs_prompt.h"
#include "stubs_queue.h"
#include "stubs_resources.h"
#include "stubs_status_bar_layer.h"
#include "stubs_syscalls.h"
#include "stubs_unobstructed_area.h"

#include "fake_pebble_tasks.h"

#include "test_recognizer_impl.h"

// touch_service.c is not compiled by this suite, so these stubs stand in for it. They count
// calls and capture the subscribed handler so tests can verify
// app_window_recognizer_glue_attach_count_changed's subscribe/unsubscribe gating (0->1
// subscribes, N->N+-1 for N>=1 does not, 1->0 unsubscribes) without exercising the real
// touch_service.c (and therefore without observing its sys_touch_reset() call -- not
// re-invoking touch_service_subscribe is what rules that out).
static int s_touch_subscribe_call_count;
static int s_touch_unsubscribe_call_count;
static TouchServiceHandler s_touch_subscribe_captured_handler;

void touch_service_subscribe(TouchServiceHandler handler, void *context) {
  s_touch_subscribe_call_count++;
  s_touch_subscribe_captured_handler = handler;
}
void touch_service_unsubscribe(void) {
  s_touch_unsubscribe_call_count++;
}

// Minimal collaborator overrides for the real window/window_stack/modal_manager closure
// (mirrors test_window_stack.c); none of these paths are under test here
void launcher_task_add_callback(void (*callback)(void *data), void *data) {
  callback(data);
}

void app_idle_timeout_pause(void) {}

void app_idle_timeout_resume(void) {}

bool layer_is_status_bar_layer(Layer *layer) {
  return false;
}

GDrawState graphics_context_get_drawing_state(GContext *ctx) {
  return (GDrawState){};
}

void graphics_context_set_drawing_state(GContext *ctx, GDrawState draw_state) {}

void compositor_transition(const CompositorTransition *type) {}

void *compositor_modal_transition_to_modal_get(bool dest) {
  return NULL;
}

void compositor_modal_render_ready(void) {}

void compositor_transition_cancel(void) {}

bool sys_app_is_watchface(void) {
  return false;
}

void click_manager_init(ClickManager *click_manager) {}

void click_manager_clear(ClickManager *click_manager) {}

void watchface_reset_click_manager(void) {}

static const WindowTransitionImplementation s_no_transition = {};

const WindowTransitionImplementation *window_transition_get_default_pop_implementation(void) {
  return &s_no_transition;
}

const WindowTransitionImplementation *window_transition_get_default_push_implementation(void) {
  return &s_no_transition;
}

const WindowTransitionImplementation g_window_transition_none_implementation = {};

static RecognizerList *s_app_list;
static RecognizerManager *s_manager;
static TestImplData s_test_impl_data;

// App-state fixtures: the real app_state.c is never compiled in unit suites. The manager
// fixture is NULL-able so tests can model a task with no recognizer manager.
RecognizerList *app_state_get_recognizer_list(void) {
  return s_app_list;
}

RecognizerManager *app_state_get_recognizer_manager(void) {
  return s_manager;
}

// Task 8: app_window_recognizer_glue.c's per-app subscription state fixture. Not in
// stubs_app_state.h (Section 2.3 rule): only this suite compiles the glue.
static AppWindowRecognizerGlueState s_recognizer_glue_state;

AppWindowRecognizerGlueState *app_state_get_recognizer_glue_state(void) {
  return &s_recognizer_glue_state;
}

typedef struct RecognizerHandled {
  ListNode node;
  int idx;
} RecognizerHandled;

static ListNode *s_recognizers_handled;
static ListNode *s_recognizers_reset;

static bool prv_simultaneous_with_cb(const Recognizer *recognizer,
                                     const Recognizer *simultaneous_with) {
  return true;
}

static void prv_handle_touch_event (Recognizer *recognizer, const TouchEvent *touch_event) {

}

static bool prv_cancel(Recognizer *recognizer) {
  return false;
}

static void prv_reset (Recognizer *recognizer) {

}

static RecognizerImpl s_dummy_impl;

static void prv_clear_recognizers_processed(ListNode **list) {
  ListNode *node = *list;
  while (node) {
    ListNode *next = list_pop_head(node);
    free(node);
    node = next;
  }
  *list = NULL;
}

static void prv_compare_recognizers_processed(int indices[], uint32_t count, ListNode **list) {
  printf(list == &s_recognizers_handled ? "Handle touch: " : "");
  printf(list == &s_recognizers_reset ? "Reset: " : "");
  printf("{ ");
  for (uint32_t i = 0; i < count; i++) {
    printf("%d, ", indices[i]);
  }
  printf("}");

  ListNode *node = *list;
  int list_num = list_count(node);
  bool failed = list_num != count;
  if (failed) {
    count = list_num;
  }

  if (!failed) {
    for (uint32_t i = 0; (i < count) && !failed; i++) {
      failed = (indices[i] != ((RecognizerHandled *)node)->idx);
      node = list_get_next(node);
    }
  }
  if (failed) {
    node = *list;
    printf(" != { ");
    for (uint32_t i = 0; i < count; i++) {
      printf("%d, ", ((RecognizerHandled *)node)->idx);
      node = list_get_next(node);
    }
    printf("}");
  }
  printf("\n");
  cl_assert(!failed);

  prv_clear_recognizers_processed(list);
}

static void prv_sub_event_handler(const Recognizer *recognizer, RecognizerEvent event) {

}

// setup and teardown
void test_recognizer_manager__initialize(void) {
  // The real window_get_recognizer_manager selects the manager by task; run as the app task
  stub_pebble_tasks_set_current(PebbleTask_App);
  s_test_impl_data = (TestImplData){};
  s_app_list = NULL;
  s_manager = NULL;
  s_recognizer_glue_state = (AppWindowRecognizerGlueState){};
  s_touch_subscribe_call_count = 0;
  s_touch_unsubscribe_call_count = 0;
  s_touch_subscribe_captured_handler = NULL;
  *app_state_get_touch_service_state() = (TouchServiceState){};
  s_dummy_impl = (RecognizerImpl) {
    .handle_touch_event = prv_handle_touch_event,
    .cancel = prv_cancel,
    .reset = prv_reset,
  };
}

void test_recognizer_manager__cleanup(void) {
  prv_clear_recognizers_processed(&s_recognizers_handled);
  prv_clear_recognizers_processed(&s_recognizers_reset);
}

static void prv_store_recognizer_idx(Recognizer *recognizer, ListNode **list) {
  int *idx = recognizer_get_impl_data(recognizer, &s_dummy_impl);
  if (idx) {
    RecognizerHandled *rec = malloc(sizeof(RecognizerHandled));
    cl_assert(rec);
    *rec = (RecognizerHandled){ .idx = *idx };
    *list = list_get_head(list_append(*list, &rec->node));
  }
}

static bool prv_handle_dummy_touch_event(Recognizer *recognizer, void *unused) {
  prv_store_recognizer_idx(recognizer, &s_recognizers_handled);
  return true;
}

static Recognizer **prv_create_recognizers(int count) {
  Recognizer **recognizers = malloc(sizeof(Recognizer*) * count);
  cl_assert(recognizers);
  for (int i = 0; i < count; i++) {
    recognizers[i] = recognizer_create_with_data(&s_dummy_impl, &i,
                                                 sizeof(i), prv_sub_event_handler,
                                                 NULL);
    cl_assert(recognizers[i]);
  }
  return recognizers;
}

static void prv_destroy_recognizers(Recognizer **recognizers, int count) {
  for (int i = 0; i < count; i++) {
    recognizer_destroy(recognizers[i]);
  }
  free(recognizers);
}

// Real-hit-test geometry: frames are in the parent's coordinate space; the real
// layer_find_layer_containing_point resolves the touchdown coordinates below to the named
// layer (layer_c nests inside layer_a; POINT_MISS lands on the root layer only)
#define ROOT_FRAME GRect(0, 0, 144, 168)
#define LAYER_A_FRAME GRect(0, 0, 50, 50)
#define LAYER_B_FRAME GRect(60, 0, 50, 50)
#define LAYER_C_FRAME GRect(10, 10, 20, 20)
#define POINT_IN_A GPoint(40, 40)
#define POINT_IN_B GPoint(70, 20)
#define POINT_IN_C GPoint(15, 15)
#define POINT_MISS GPoint(100, 120)

static void prv_set_touch_pos(TouchEvent *e, GPoint pos) {
  e->x = pos.x;
  e->y = pos.y;
}
// tests



bool prv_process_all_recognizers(RecognizerManager *manager,
                                 RecognizerListIteratorCb iter_cb, void *context);

void test_recognizer_manager__process_all_recognizers(void) {
  const int k_rec_count = 7;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);
  RecognizerManager manager;
  recognizer_manager_init(&manager);

  // ensure this runs without crashing even if there are no recognizer lists
  prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL);

  RecognizerList app_list = {};
  s_app_list = &app_list;
  Window window = {};
  layer_init(&window.layer, &GRectZero);
  manager.window = &window;

  Layer layer_a, layer_b, layer_c;
  layer_init(&layer_a, &GRectZero);
  layer_init(&layer_b, &GRectZero);
  layer_init(&layer_c, &GRectZero);
  layer_add_child(&window.layer, &layer_a);
  layer_add_child(&layer_a, &layer_b);
  layer_add_child(&window.layer, &layer_c);
  manager.active_layer = &layer_c;

  // ensure that this runs without crashing even if all the lists are empty
  prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL);

  // One recognizer attached to the active layer
  recognizer_add_to_list(recognizers[0], &layer_c.recognizer_list);
  cl_assert(prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL));
  prv_compare_recognizers_processed((int[]) {0}, 1, &s_recognizers_handled);

  // Two recognizers attached to the active layer - processed in order that they were added
  recognizer_add_to_list(recognizers[1], &layer_c.recognizer_list);
  cl_assert(prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL));
  prv_compare_recognizers_processed((int[]) {0, 1}, 2, &s_recognizers_handled);

  // Recognizers that attached to layers other than the active layer and its ancestors will not be
  // processed
  recognizer_add_to_list(recognizers[2], &layer_a.recognizer_list);
  recognizer_add_to_list(recognizers[3], &layer_a.recognizer_list);
  recognizer_add_to_list(recognizers[4], &layer_b.recognizer_list);
  cl_assert(prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL));
  prv_compare_recognizers_processed((int[]) {0, 1}, 2, &s_recognizers_handled);

  // Recognizers attached to children of active layer will not be evaluated
  manager.active_layer = &layer_a;
  cl_assert(prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL));
  prv_compare_recognizers_processed((int[]) {2, 3}, 2, &s_recognizers_handled);

  // Recognizers attached to active layer will be processed before those attached to their ancestors
  manager.active_layer = &layer_b;
  cl_assert(prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL));
  prv_compare_recognizers_processed((int[]) {4, 2, 3}, 3, &s_recognizers_handled);

  // Recognizers attached to window processed before layer recognizers
  recognizer_add_to_list(recognizers[5], window_get_recognizer_list(&window));
  cl_assert(prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL));
  prv_compare_recognizers_processed((int[]) {5, 4, 2, 3}, 4, &s_recognizers_handled);

  // Recognizers attached to app processed before window and layer recognizers
  recognizer_add_to_list(recognizers[6], &app_list);
  cl_assert(prv_process_all_recognizers(&manager, prv_handle_dummy_touch_event, NULL));
  prv_compare_recognizers_processed((int[]) {6, 5, 4, 2, 3}, 5, &s_recognizers_handled);

  prv_destroy_recognizers(recognizers, k_rec_count);
}

bool prv_dispatch_touch_event(Recognizer *recognizer, void *context);

void test_recognizer_manager__dispatch_touch_event(void) {
  bool handled = false;
  s_test_impl_data.handled = &handled;
  NEW_RECOGNIZER(r) = test_recognizer_create(&s_test_impl_data, NULL);

  // Copied from recognizer_manager.c
  TouchEvent t;
  struct ProcessTouchCtx {
    Recognizer *triggered;
    const TouchEvent *touch_event;
  } ctx = { .triggered = NULL, .touch_event = &t };

  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(handled);
  cl_assert(!ctx.triggered);

  handled = false;
  // Recognizer should not get a touch event when it is in inactive states
  r->state = RecognizerState_Failed;
  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(!handled);
  cl_assert(!ctx.triggered);

  r->state = RecognizerState_Cancelled;
  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(!handled);
  cl_assert(!ctx.triggered);

  r->state = RecognizerState_Completed;
  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(!handled);
  cl_assert(!ctx.triggered);

  r->state = RecognizerState_Started;
  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(handled);
  cl_assert_equal_p(ctx.triggered, r);
  ctx.triggered = NULL;
  handled = false;

  r->state = RecognizerState_Updated;
  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(handled);
  cl_assert_equal_p(ctx.triggered, r);
  handled = false;
  ctx.triggered = NULL;

  NEW_RECOGNIZER(s) = test_recognizer_create(&s_test_impl_data, NULL);
  s->state = RecognizerState_Started;
  r->state = RecognizerState_Possible;
  ctx.triggered = s;
  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(!handled);

  recognizer_set_simultaneous_with(r, prv_simultaneous_with_cb);
  cl_assert(prv_dispatch_touch_event(r, &ctx));
  cl_assert(handled);
  cl_assert_equal_p(ctx.triggered, s);
}


bool prv_fail_recognizer(Recognizer *recognizer, void *context);

void test_recognizer_manager__fail_recognizer(void) {
  NEW_RECOGNIZER(r1) = test_recognizer_create(&s_test_impl_data, NULL);
  NEW_RECOGNIZER(r2) = test_recognizer_create(&s_test_impl_data, NULL);
  r2->state = RecognizerState_Started;

  // copied from recognizer_manager.c
  struct FailRecognizerCtx {
    Recognizer *triggered;
    bool recognizers_active;
  } ctx = { .triggered = r2, .recognizers_active = false };

  cl_assert(prv_fail_recognizer(r2, &ctx));
  cl_assert_equal_i(r2->state, RecognizerState_Started);
  cl_assert(!ctx.recognizers_active);

  ctx.recognizers_active = false;
  r1->state = RecognizerState_Possible;
  cl_assert(prv_fail_recognizer(r1, &ctx));
  cl_assert_equal_i(r1->state, RecognizerState_Failed);
  cl_assert(!ctx.recognizers_active);

  // Make sure that we don't try to fail a recognizer twice (causing an assert)
  cl_assert(prv_fail_recognizer(r1, &ctx));
  cl_assert_equal_i(r1->state, RecognizerState_Failed);

  r1->state = RecognizerState_Possible;
  recognizer_set_simultaneous_with(r1, prv_simultaneous_with_cb);
  cl_assert(prv_fail_recognizer(r1, &ctx));
  cl_assert_equal_i(r1->state, RecognizerState_Possible);
  cl_assert(ctx.recognizers_active);

}


void prv_cancel_layer_tree_recognizers(RecognizerManager *manager, Layer *top_layer,
                                       Layer *bottom_layer);

static void prv_set_all_states(Recognizer **recognizers, int count, RecognizerState state) {
  for(int i = 0; i < count; i++) {
    recognizers[i]->state = state;
  }
}

void test_recognizer_manager__cancel_layer_tree_recognizers(void) {
  const int k_rec_count = 4;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &GRectZero);
  Layer *root = &window.layer;
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;

  Layer layer_a, layer_b, layer_c;
  layer_init(&layer_a, &GRectZero);
  layer_init(&layer_b, &GRectZero);
  layer_init(&layer_c, &GRectZero);
  layer_add_child(root, &layer_a);
  layer_add_child(root, &layer_b);
  layer_add_child(&layer_a, &layer_c);

  recognizer_add_to_list(recognizers[0], window_get_recognizer_list(&window));
  recognizer_add_to_list(recognizers[1], &layer_a.recognizer_list);
  recognizer_add_to_list(recognizers[2], &layer_b.recognizer_list);
  recognizer_add_to_list(recognizers[3], &layer_c.recognizer_list);

  prv_set_all_states(recognizers, k_rec_count, RecognizerState_Started);

  // Layer C's recognizers reset when layer A becomes the new active layer
  manager.active_layer = &layer_c;
  prv_cancel_layer_tree_recognizers(&manager, &layer_a, &layer_c);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[2]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Cancelled);

  // Layer C's and layer A's recognizers get reset when layer B becomes the new active layer
  prv_set_all_states(recognizers, k_rec_count, RecognizerState_Started);
  prv_cancel_layer_tree_recognizers(&manager, &layer_b, &layer_c);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Cancelled);
  cl_assert_equal_i(recognizers[2]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Cancelled);

  // Layer C's and layer A's recognizers get cancelled when there is no new active layer
  prv_set_all_states(recognizers, k_rec_count, RecognizerState_Started);
  prv_cancel_layer_tree_recognizers(&manager, NULL, &layer_c);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Cancelled);
  cl_assert_equal_i(recognizers[2]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Cancelled);

  // If recognizers are in the possible state, they will be failed, rather than cancelled
  prv_set_all_states(recognizers, k_rec_count, RecognizerState_Possible);
  prv_cancel_layer_tree_recognizers(&manager, NULL, &layer_c);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[2]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Failed);

}

static RecognizerState s_next_state = RecognizerStateCount;
static int s_idx_to_change = -1;
static void prv_handle_touch_event_test(Recognizer *recognizer, const TouchEvent *touch_event) {
  int *idx = recognizer_get_impl_data(recognizer, &s_dummy_impl);
  prv_store_recognizer_idx(recognizer, &s_recognizers_handled);
  if ((s_idx_to_change >= 0) && (*idx == s_idx_to_change)) {
    recognizer_transition_state(recognizer, s_next_state);
    s_idx_to_change = -1;
    s_next_state = RecognizerStateCount;
  }
}

static void prv_reset_test(Recognizer *recognizer) {
  prv_store_recognizer_idx(recognizer, &s_recognizers_reset);
}

void test_recognizer_manager__handle_touch_event(void) {
  const int k_rec_count = 5;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  RecognizerList app_list = {};
  s_app_list = &app_list;

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  Layer *root = &window.layer;
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;

  Layer layer_a, layer_b, layer_c;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_init(&layer_b, &LAYER_B_FRAME);
  layer_init(&layer_c, &LAYER_C_FRAME);
  layer_add_child(root, &layer_a);
  layer_add_child(root, &layer_b);
  layer_add_child(&layer_a, &layer_c);

  recognizer_add_to_list(recognizers[0], window_get_recognizer_list(&window));
  recognizer_add_to_list(recognizers[1], &layer_a.recognizer_list);
  recognizer_add_to_list(recognizers[2], &layer_b.recognizer_list);
  recognizer_add_to_list(recognizers[3], &layer_c.recognizer_list);
  recognizer_add_to_list(recognizers[4], s_app_list);

  TouchEvent e = { .type = TouchEvent_PositionUpdate };
  prv_set_touch_pos(&e, POINT_IN_C);

  // No active recognizers because manager is waiting for a touchdown event
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_handled);

  // Touchdown event occurs, active layer is found and all applicable recognizers receive events
  // while none have started recognizing
  e.type = TouchEvent_Touchdown;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_handled);
  cl_assert_equal_p(manager.active_layer, &layer_c);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // All recognizers receive events while none have started recognizing
  e.type = TouchEvent_PositionUpdate;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_handled);
  cl_assert_equal_p(manager.active_layer, &layer_c);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // Task 8 (impl plan section 12 + understanding-touchscreen-tasks-8-10, operator decision 1):
  // this assertion previously pinned "manager stays RecognizersActive after Liftoff" -- that
  // was the gap the Liftoff branch closes, not a contract to preserve: with every recognizer
  // still Possible, nothing completed the gesture, so the manager must return to
  // WaitForTouchdown instead of latching active_layer across strokes. Rewritten (not
  // deleted) per the Task 4 assertion-preservation discipline, with this justification.
  e.type = TouchEvent_Liftoff;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]){4, 0, 3, 1}, 4, &s_recognizers_reset);
  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // Layer A recognizer's gesture starts to be recognized. All other recognizers failed
  e.type = TouchEvent_Touchdown;
  s_next_state = RecognizerState_Started;
  s_idx_to_change = 3;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 3}, 3, &s_recognizers_handled);
  cl_assert_equal_p(manager.active_layer, &layer_c);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Failed);

  // Only layer A recognizer's gesture receives touch events
  e.type = TouchEvent_PositionUpdate;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {3}, 1, &s_recognizers_handled);
  cl_assert_equal_p(manager.active_layer, &layer_c);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Failed);

  // Layer A recognizer's gesture updates. Only that recognizer receives touch events
  e.type = TouchEvent_Liftoff;
  s_next_state = RecognizerState_Updated;
  s_idx_to_change = 3;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {3}, 1, &s_recognizers_handled);
  cl_assert_equal_p(manager.active_layer, &layer_c);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Updated);

  // Layer A recognizer's gesture completes and all recognizers are reset
  e.type = TouchEvent_Liftoff;
  s_next_state = RecognizerState_Completed;
  s_idx_to_change = 3;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {3}, 1, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[2]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // Layer A recognizer's gesture does not complete because there is no active layer until a
  // touchdown occurs
  e.type = TouchEvent_PositionUpdate;
  s_next_state = RecognizerState_Completed;
  s_idx_to_change = 3;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);

  // Layer A's recognizer's gesture completes immediately. All recognizers receive the touch event
  // because Layer A's recognizers receive the touch events last. All recognizers in the chain are
  // reset
  e.type = TouchEvent_Touchdown;
  s_next_state = RecognizerState_Completed;
  s_idx_to_change = 1;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);

  // The app's recognizer's gesture completes immediately. Only the app's recognizer sees the touch
  // events. All recognizers in the chain are reset
  e.type = TouchEvent_Touchdown;
  s_next_state = RecognizerState_Completed;
  s_idx_to_change = 4;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4}, 1, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);

  // Layer C recognizer starts recognizing a gesture, failing other recognizers
  e.type = TouchEvent_Touchdown;
  s_next_state = RecognizerState_Started;
  s_idx_to_change = 1;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Failed);

  // A second touchdown event occurs while recognizers are active. A different layer is touched, so
  // the active recognizers on non-touched layers in the tree are cancelled
  prv_set_touch_pos(&e, POINT_IN_B);
  e.type = TouchEvent_Touchdown;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 2}, 3, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]) {4, 0, 2}, 3, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Cancelled);
  cl_assert_equal_i(recognizers[2]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // Window recognizer becomes triggered
  e.type = TouchEvent_PositionUpdate;
  s_next_state = RecognizerState_Started;
  s_idx_to_change = 0;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0 }, 2, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[2]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Failed);

  // Another layer in a separate branch becomes active while a window recognizer is triggered
  e.type = TouchEvent_Touchdown;
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) { 0 }, 1, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Cancelled); // was already cancelled
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Failed);

  // A child layer of the active layer becomes active when a window recognizer is triggered
  e.type = TouchEvent_Touchdown;
  prv_set_touch_pos(&e, POINT_IN_C);
  recognizers[3]->state = RecognizerState_Possible;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) { 0 }, 1, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Cancelled);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Failed);

  // A touchdown occurs where no layers are touched while a window recognizer is active
  e.type = TouchEvent_Touchdown;
  prv_set_touch_pos(&e, POINT_MISS);
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) { 0 }, 1, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Cancelled);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Failed);

  // Touchdown occurs, Window recognizer completes, active layer becomes non-null
  e.type = TouchEvent_Touchdown;
  s_next_state = RecognizerState_Completed;
  s_idx_to_change = 0;
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) { 0 }, 1, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]) { 4, 0, 1 }, 3, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // A touchdown occurs where no layers are touched
  prv_set_touch_pos(&e, POINT_MISS);
  e.type = TouchEvent_Touchdown;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0}, 2, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // A touchdown occurs and the active layer goes from non-null to null. All layer recognizers get
  // reset. All recognizers remain in the possible state.
  prv_set_touch_pos(&e, POINT_IN_A);
  e.type = TouchEvent_Touchdown;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 1}, 3, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]) {1}, 1, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // A touchdown occurs and a child of the previous active recognizer becomes the active layer. The
  // child is reset. All recognizers remain in the possible state.
  prv_set_touch_pos(&e, POINT_IN_C);
  e.type = TouchEvent_Touchdown;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 3, 1}, 4, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]) {3}, 1, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  // A touchdown occurs and the parent of the previous active recognizer becomes the active layer.
  // No recognizers are reset and all recognizers remain in the possible state. The child is failed.
  prv_set_touch_pos(&e, POINT_IN_A);
  e.type = TouchEvent_Touchdown;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]) {4, 0, 1}, 3, &s_recognizers_handled);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[3]->state, RecognizerState_Failed);
  cl_assert_equal_i(recognizers[4]->state, RecognizerState_Possible);

  prv_destroy_recognizers(recognizers, k_rec_count);
}

// A window-stack transition slides a window by writing a nonzero frame origin on the ROOT layer
// (window_stack_animation_rect.c prv_window_frame_setter interpolates it across the slide;
// window_stack_animation_round.c writes it directly). Touch events are NOT gated on
// window_stack_is_animating -- the two gates on the button path both sit inside
// launcher_handle_button_event, which the touch path never reaches -- so the manager really does
// resolve touchdowns while the root origin is mid-interpolation.
//
// This matters more than a single mis-hit: recognizer_manager_handle_touch_event latches
// active_layer once, on Touchdown, and holds it for the remainder of the stroke. A wrong tree walk
// here misroutes every subsequent event in the drag.
//
// ROOT_FRAME above has origin (0,0) and LAYER_A_FRAME is at (0,0) too, so no other case in this
// suite exercises the root rebase. This one uses its own frame rather than changing the shared
// macro, which would perturb every other case here.
#define SLID_ROOT_FRAME GRect(20, 0, 144, 168)

void test_recognizer_manager__handle_touch_event_with_slid_root(void) {
  const int k_rec_count = 2;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  RecognizerList app_list = {};
  s_app_list = &app_list;

  Window window = {};
  layer_init(&window.layer, &SLID_ROOT_FRAME);
  Layer *root = &window.layer;
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;

  Layer layer_a, layer_c;
  layer_init(&layer_a, &LAYER_A_FRAME);   // (0,0,50,50), window-local
  layer_init(&layer_c, &LAYER_C_FRAME);   // (10,10,20,20), nested inside layer_a
  layer_add_child(root, &layer_a);
  layer_add_child(&layer_a, &layer_c);

  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);
  recognizer_add_to_list(recognizers[1], &layer_c.recognizer_list);

  // layer_c occupies window-local (10,10)-(30,30), which is screen (30,10)-(50,30) once the root
  // is slid 20px right. Discriminating: screen (35,15) is window-local (15,15), inside layer_c.
  // Without the root rebase the traversal tests the raw screen x=35 against layer_c's [10,30)
  // frame, misses, and latches layer_a for the whole stroke instead.
  TouchEvent e = { .type = TouchEvent_Touchdown };
  prv_set_touch_pos(&e, GPoint(35, 15));
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_p(manager.active_layer, &layer_c);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);

  // Screen x=25 is window-local x=5: inside layer_a, before layer_c starts.
  prv_set_touch_pos(&e, GPoint(25, 15));
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_p(manager.active_layer, &layer_a);

  // Screen x=15 is left of the slid window entirely: no layer, so the manager reports none.
  prv_set_touch_pos(&e, GPoint(15, 15));
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_p(manager.active_layer, NULL);

  prv_destroy_recognizers(recognizers, k_rec_count);
}

void test_recognizer_manager__ownership_survives_reset_and_teardown(void) {
  bool destroyed = false;
  bool updated = false;
  RecognizerState new_state = RecognizerState_Possible;
  s_test_impl_data.destroyed = &destroyed;
  s_test_impl_data.updated = &updated;
  s_test_impl_data.new_state = &new_state;
  Recognizer *r = test_recognizer_create(&s_test_impl_data, NULL);
  test_recognizer_enable_on_destroy();

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;
  s_manager = &manager;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window.layer, &layer_a);

  layer_attach_recognizer(&layer_a, r);
  cl_assert(recognizer_is_owned(r));
  cl_assert_equal_p(recognizer_get_manager(r), &manager);

  // Complete one full gesture: the recognizer transitions to Completed on touchdown, so the
  // manager runs its reset-all path (prv_fail_then_reset_if_no_active_recognizers)
  new_state = RecognizerState_Completed;
  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);

  // The manager-driven reset ran to completion...
  cl_assert_equal_b(updated, true);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  // ...and must not have clobbered list ownership
  cl_assert(recognizer_is_owned(r));

  // Teardown: layer_deinit destroys the recognizer exactly once and unlinks it from the list.
  // Without ownership intact, recognizer_remove_from_list early-returns and layer_deinit
  // iterates a list that still links the freed recognizer.
  layer_deinit(&layer_a);
  cl_assert_equal_b(destroyed, true);
  cl_assert_equal_p(layer_a.recognizer_list.node, NULL);
}

void test_recognizer_manager__public_reset_clears_manager_fields(void) {
  bool updated = false;
  bool cancelled = false;
  bool destroyed = false;
  RecognizerState new_state = RecognizerState_Started;
  s_test_impl_data.updated = &updated;
  s_test_impl_data.cancelled = &cancelled;
  s_test_impl_data.destroyed = &destroyed;
  s_test_impl_data.new_state = &new_state;
  Recognizer *r = test_recognizer_create(&s_test_impl_data, NULL);
  test_recognizer_enable_on_destroy();

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;
  s_manager = &manager;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window.layer, &layer_a);
  layer_attach_recognizer(&layer_a, r);

  // Drive the manager into RecognizersTriggered: the recognizer transitions to Started when it
  // handles the touchdown
  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);

  cl_assert_equal_b(updated, true);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_p(manager.triggered, r);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);

  // The public reset must perform the full reset the header contract describes
  recognizer_manager_reset(&manager);

  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  // The Started recognizer was cancelled on its way back to Possible
  cl_assert_equal_b(cancelled, true);

  layer_deinit(&layer_a);
  cl_assert_equal_b(destroyed, true);
}

void test_recognizer_manager__cancel_touches_cancels_live_recognizer(void) {
  bool cancelled = false;
  bool updated = false;
  RecognizerState new_state = RecognizerState_Started;
  s_test_impl_data.cancelled = &cancelled;
  s_test_impl_data.updated = &updated;
  s_test_impl_data.new_state = &new_state;
  NEW_RECOGNIZER(r) = test_recognizer_create(&s_test_impl_data, NULL);

  RecognizerList app_list = {};
  s_app_list = &app_list;
  recognizer_add_to_list(r, &app_list);

  // App-list recognizer, dispatched via a manager pointed at a real (empty) window. Task 8's
  // NULL-window guard on recognizer_manager_handle_touch_event makes manager->window == NULL a
  // hard no-op (impl plan section 12 + operator disambiguation answer 2), so this arrange step
  // now needs a real window for the touchdown to reach the app-list recognizer at all; the
  // assertions below (app-list dispatch, cancel_touches semantics) are unchanged.
  Window window = {};
  layer_init(&window.layer, &GRectZero);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;

  // The recognizer transitions to Started when it handles the touchdown, so the
  // manager records it as the triggered recognizer
  TouchEvent e = {.type = TouchEvent_Touchdown};
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_b(updated, true);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Started);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_p(manager.triggered, r);

  // cancel_touches must cancel the live recognizer and release it as triggered
  recognizer_manager_cancel_touches(&manager);
  cl_assert_equal_b(cancelled, true);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Cancelled);
  cl_assert_equal_p(manager.triggered, NULL);

  recognizer_remove_from_list(r, &app_list);
}

void test_recognizer_manager__deregister_recognizer(void) {
  NEW_RECOGNIZER(r1) = test_recognizer_create(&s_test_impl_data, NULL);
  NEW_RECOGNIZER(r2) = test_recognizer_create(&s_test_impl_data, NULL);

  Window window = {};
  layer_init(&window.layer, &GRectZero);
  Layer *root = &window.layer;
  RecognizerManager manager;
  recognizer_manager_init(&manager);

  Layer layer_a;
  layer_init(&layer_a, &GRectZero);
  layer_add_child(root, &layer_a);

  manager.window = &window;
  manager.active_layer = &layer_a;

  recognizer_add_to_list(r1, &layer_a.recognizer_list);
  recognizer_add_to_list(r2, &layer_a.recognizer_list);

  RecognizerManager manager2;
  recognizer_set_manager(r1, &manager2);

  recognizer_manager_deregister_recognizer(&manager, r1);
  cl_assert_equal_p(manager.active_layer,  &layer_a);
  cl_assert_equal_p(recognizer_get_manager(r1), &manager2);

  recognizer_set_manager(r1, &manager);

  recognizer_manager_deregister_recognizer(&manager, r1);
  cl_assert(!recognizer_get_manager(r1));
  cl_assert_equal_p(manager.active_layer, &layer_a);

  recognizer_set_manager(r1, &manager);
  r1->state = RecognizerState_Started;
  r2->state = RecognizerState_Failed;
  manager.triggered = r1;
  manager.state = RecognizerManagerState_RecognizersTriggered;
  recognizer_manager_deregister_recognizer(&manager, r1);
  cl_assert(!manager.triggered);
  cl_assert(!manager.active_layer);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_i(r2->state, RecognizerState_Possible);
  cl_assert(!recognizer_get_manager(r1));

  recognizer_set_manager(r1, &manager);
  r1->state = RecognizerState_Possible;
  r2->state = RecognizerState_Started;
  manager.active_layer = &layer_a;
  manager.triggered = r2;
  manager.state = RecognizerManagerState_RecognizersTriggered;
  recognizer_manager_deregister_recognizer(&manager, r1);
  cl_assert_equal_p(manager.triggered, r2);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(r2->state, RecognizerState_Started);
  cl_assert(!recognizer_get_manager(r1));

  recognizer_set_manager(r1, &manager);
  recognizer_set_simultaneous_with(r2, prv_simultaneous_with_cb);
  r1->state = RecognizerState_Started;
  r2->state = RecognizerState_Started;
  manager.triggered = r1;
  manager.state = RecognizerManagerState_RecognizersTriggered;
  recognizer_manager_deregister_recognizer(&manager, r1);
  cl_assert_equal_p(manager.triggered, r2);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(r2->state, RecognizerState_Started);
  cl_assert(!recognizer_get_manager(r1));
}

void test_recognizer_manager__handle_state_change(void) {
  const int k_rec_count = 2;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **r = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &GRectZero);
  Layer *root = &window.layer;
  RecognizerManager manager;
  recognizer_manager_init(&manager);

  Layer layer_a;
  layer_init(&layer_a, &GRectZero);
  layer_add_child(root, &layer_a);

  manager.window = &window;
  manager.active_layer = &layer_a;
  manager.state = RecognizerManagerState_RecognizersActive;

  recognizer_add_to_list(r[0], &layer_a.recognizer_list);
  recognizer_add_to_list(r[1], &layer_a.recognizer_list);

  recognizer_set_manager(r[0], &manager);
  recognizer_set_manager(r[1], &manager);

  r[0]->state = RecognizerState_Failed;
  recognizer_manager_handle_state_change(&manager, r[0]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);

  r[1]->state = RecognizerState_Failed;
  recognizer_manager_handle_state_change(&manager, r[1]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.active_layer, NULL);
  prv_compare_recognizers_processed((int []) { 0, 1 }, 2, &s_recognizers_reset);

  manager.active_layer = &layer_a;
  manager.state = RecognizerManagerState_RecognizersActive;
  manager.triggered = NULL;
  r[0]->state = RecognizerState_Started;
  r[1]->state = RecognizerState_Possible;
  recognizer_manager_handle_state_change(&manager, r[0]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_p(manager.triggered, r[0]);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(r[0]->state, RecognizerState_Started);
  cl_assert_equal_i(r[1]->state, RecognizerState_Failed);

  r[0]->state = RecognizerState_Updated;
  recognizer_manager_handle_state_change(&manager, r[0]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_p(manager.triggered, r[0]);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);

  r[0]->state = RecognizerState_Completed;
  recognizer_manager_handle_state_change(&manager, r[0]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_p(manager.active_layer, NULL);
  prv_compare_recognizers_processed((int []) { 0, 1 }, 2, &s_recognizers_reset);
  cl_assert_equal_i(r[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(r[1]->state, RecognizerState_Possible);

  r[0]->state = RecognizerState_Completed;
  manager.active_layer = &layer_a;
  manager.state = RecognizerManagerState_RecognizersActive;
  manager.triggered = NULL;
  recognizer_manager_handle_state_change(&manager, r[0]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_p(manager.active_layer, NULL);
  prv_compare_recognizers_processed((int []) { 0, 1 }, 2, &s_recognizers_reset);
  cl_assert_equal_i(r[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(r[1]->state, RecognizerState_Possible);

  r[0]->state = RecognizerState_Cancelled;
  manager.active_layer = &layer_a;
  manager.state = RecognizerManagerState_RecognizersActive;
  manager.triggered = r[0];
  recognizer_manager_handle_state_change(&manager, r[0]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_p(manager.active_layer, NULL);
  prv_compare_recognizers_processed((int []) { 0, 1 }, 2, &s_recognizers_reset);

  recognizer_set_simultaneous_with(r[0], prv_simultaneous_with_cb);
  r[0]->state = RecognizerState_Started;
  r[1]->state = RecognizerState_Completed;
  manager.active_layer = &layer_a;
  manager.state = RecognizerManagerState_RecognizersTriggered;
  manager.triggered = r[0];
  recognizer_manager_handle_state_change(&manager, r[1]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_p(manager.triggered, r[0]);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(r[0]->state, RecognizerState_Started);
  cl_assert_equal_i(r[1]->state, RecognizerState_Completed);

  recognizer_set_simultaneous_with(r[0], prv_simultaneous_with_cb);
  r[0]->state = RecognizerState_Started;
  r[1]->state = RecognizerState_Completed;
  manager.active_layer = &layer_a;
  manager.state = RecognizerManagerState_RecognizersTriggered;
  manager.triggered = r[1];
  recognizer_manager_handle_state_change(&manager, r[1]);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_p(manager.triggered, r[0]);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_reset);
  cl_assert_equal_i(r[0]->state, RecognizerState_Started);
  cl_assert_equal_i(r[1]->state, RecognizerState_Completed);
}

void test_recognizer_manager__touchdown_miss_dispatches_root_recognizers(void) {
  const int k_rec_count = 2;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;
  s_manager = &manager;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window.layer, &layer_a);

  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);
  window_attach_recognizer(&window, recognizers[1]);

  // Touchdown that hits no child layer: the real hit-test resolves to the root layer, which
  // the manager records as no active layer; dispatch still reaches root-list recognizers
  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_MISS);
  recognizer_manager_handle_touch_event(&e, &manager);

  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  prv_compare_recognizers_processed((int[]){1}, 1, &s_recognizers_handled);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  window_detach_recognizer(&window, recognizers[1]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

void test_recognizer_manager__off_screen_sequence_stops_dispatch(void) {
  const int k_rec_count = 2;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  recognizer_manager_set_window(&manager, &window);
  s_manager = &manager;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window.layer, &layer_a);
  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);
  window_attach_recognizer(&window, recognizers[1]);

  // Live stroke on the window: touchdown inside layer_a reaches both recognizers
  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed((int[]){1, 0}, 2, &s_recognizers_handled);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);

  // The window leaves the screen mid-stroke: run the choke-point sequence
  recognizer_manager_cancel_touches(&manager);
  recognizer_manager_reset(&manager);
  recognizer_manager_set_window(&manager, NULL);

  // Disappear-side invariant: no manager field references the departed window
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.window, NULL);
  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_p(manager.triggered, NULL);

  // Subsequent events are a no-op: no recognizer receives them and none leaves Possible
  // (assert on event counts, not hit-test internals, so this holds with or without a
  // NULL-window guard inside recognizer_manager_handle_touch_event)
  e.type = TouchEvent_Touchdown;
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);
  e.type = TouchEvent_PositionUpdate;
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_handled);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  cl_assert_equal_i(recognizers[1]->state, RecognizerState_Possible);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  window_detach_recognizer(&window, recognizers[1]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

void test_recognizer_manager__ownership_survives_real_window_teardown(void) {
  bool destroyed = false;
  bool updated = false;
  RecognizerState new_state = RecognizerState_Completed;
  s_test_impl_data.destroyed = &destroyed;
  s_test_impl_data.updated = &updated;
  s_test_impl_data.new_state = &new_state;
  Recognizer *r = test_recognizer_create(&s_test_impl_data, NULL);
  test_recognizer_enable_on_destroy();

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;
  s_manager = &manager;

  // Put the window on screen so window_deinit exercises the real off-screen branch
  window_set_on_screen(&window, true, false);
  cl_assert_equal_b(window.on_screen, true);

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window.layer, &layer_a);
  layer_attach_recognizer(&layer_a, r);

  // Complete one full gesture through the real hit-test: the recognizer transitions to
  // Completed on touchdown, so the manager runs its reset-all path
  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_b(updated, true);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_i(recognizer_get_state(r), RecognizerState_Possible);
  // The manager-driven reset must not have clobbered list ownership
  cl_assert(recognizer_is_owned(r));

  // Real teardown: window_deinit takes the window off screen and unlinks its child layers
  window_deinit(&window);
  cl_assert_equal_b(window.on_screen, false);
  cl_assert_equal_p(window.layer.first_child, NULL);

  // The unlinked layer still owns the recognizer; layer_deinit reclaims it exactly once
  cl_assert(recognizer_is_owned(r));
  layer_deinit(&layer_a);
  cl_assert_equal_b(destroyed, true);
  cl_assert_equal_p(layer_a.recognizer_list.node, NULL);
}

void test_recognizer_manager__attach_with_null_manager_is_inert_and_owned(void) {
  s_stub_app_state_recognizer_attach_count = 0;
  bool destroyed = false;
  s_test_impl_data.destroyed = &destroyed;
  Recognizer *r = test_recognizer_create(&s_test_impl_data, NULL);
  test_recognizer_enable_on_destroy();

  Window window = {};
  layer_init(&window.layer, &GRectZero);
  // s_manager stays NULL (initialize): no recognizer manager is reachable

  Layer layer_a;
  layer_init(&layer_a, &GRectZero);
  layer_add_child(&window.layer, &layer_a);

  // Attach must not assert: register is skipped, but the layer list still owns
  // the recognizer so it stays reclaimable
  layer_attach_recognizer(&layer_a, r);
  cl_assert(recognizer_is_owned(r));
  cl_assert_equal_p(recognizer_get_manager(r), NULL);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 1);

  // The layer list is the ownership root: deinit reclaims the inert recognizer
  layer_deinit(&layer_a);
  cl_assert_equal_b(destroyed, true);
  cl_assert_equal_p(layer_a.recognizer_list.node, NULL);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);
}

void test_recognizer_manager__detach_with_null_manager_no_assert(void) {
  s_stub_app_state_recognizer_attach_count = 0;
  bool destroyed = false;
  s_test_impl_data.destroyed = &destroyed;
  Recognizer *r = test_recognizer_create(&s_test_impl_data, NULL);
  test_recognizer_enable_on_destroy();

  Window window = {};
  layer_init(&window.layer, &GRectZero);
  // s_manager stays NULL (initialize): no recognizer manager is reachable

  Layer layer_a;
  layer_init(&layer_a, &GRectZero);
  layer_add_child(&window.layer, &layer_a);

  layer_attach_recognizer(&layer_a, r);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 1);

  // Detach must not assert on the NULL manager (deregister is skipped)
  layer_detach_recognizer(&layer_a, r);
  cl_assert(!recognizer_is_owned(r));
  cl_assert_equal_p(layer_a.recognizer_list.node, NULL);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);

  recognizer_destroy(r);
  cl_assert_equal_b(destroyed, true);
}

void test_recognizer_manager__double_attach_does_not_double_count(void) {
  s_stub_app_state_recognizer_attach_count = 0;
  bool destroyed = false;
  s_test_impl_data.destroyed = &destroyed;
  Recognizer *r = test_recognizer_create(&s_test_impl_data, NULL);
  test_recognizer_enable_on_destroy();

  Window window = {};
  layer_init(&window.layer, &GRectZero);
  // s_manager stays NULL (initialize): no recognizer manager is reachable

  Layer layer_a;
  layer_init(&layer_a, &GRectZero);
  layer_add_child(&window.layer, &layer_a);
  Layer layer_b;
  layer_init(&layer_b, &GRectZero);
  layer_add_child(&window.layer, &layer_b);

  layer_attach_recognizer(&layer_a, r);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 1);

  // Second attach is an ownership no-op (recognizer_add_to_list early-returns on
  // is_owned): the counter must not drift to 2 with only one listed recognizer
  layer_attach_recognizer(&layer_b, r);
  cl_assert(recognizer_is_owned(r));
  cl_assert_equal_p(layer_b.recognizer_list.node, NULL);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 1);

  // Deinit of the owning layer releases the one real attachment: count returns to
  // zero, not stuck at 1
  layer_deinit(&layer_a);
  cl_assert_equal_b(destroyed, true);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);

  layer_deinit(&layer_b);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);
}

void test_recognizer_manager__detach_never_attached_does_not_decrement(void) {
  s_stub_app_state_recognizer_attach_count = 0;
  bool destroyed = false;
  s_test_impl_data.destroyed = &destroyed;
  Recognizer *r = test_recognizer_create(&s_test_impl_data, NULL);
  test_recognizer_enable_on_destroy();

  Window window = {};
  layer_init(&window.layer, &GRectZero);
  // s_manager stays NULL (initialize): no recognizer manager is reachable

  Layer layer_a;
  layer_init(&layer_a, &GRectZero);
  layer_add_child(&window.layer, &layer_a);

  // Detach of a never-attached recognizer is an ownership no-op
  // (recognizer_remove_from_list early-returns on !is_owned): the counter must
  // not decrement below the number of real attachments
  layer_detach_recognizer(&layer_a, r);
  cl_assert(!recognizer_is_owned(r));
  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);

  recognizer_destroy(r);
  cl_assert_equal_b(destroyed, true);
}

////////////////////////////////////////////////////////////////////////////////////////////////
// Task 8 (impl plan section 12 + understanding-touchscreen-tasks-8-10): NULL-window guard,
// the Liftoff branch, and the app_window_recognizer_glue.c lifecycle wiring.

void test_recognizer_manager__null_window_dispatch_is_noop(void) {
  const int k_rec_count = 1;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  RecognizerList app_list = {};
  s_app_list = &app_list;
  recognizer_add_to_list(recognizers[0], &app_list);

  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = NULL;
  // Intentionally poisoned: a non-NULL sentinel the guard must never dereference or overwrite.
  manager.active_layer = (Layer *)0x1;

  TouchEvent e = {.type = TouchEvent_Touchdown};
  recognizer_manager_handle_touch_event(&e, &manager);

  prv_compare_recognizers_processed(NULL, 0, &s_recognizers_handled);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.active_layer, (Layer *)0x1);

  // A NULL manager pointer itself must also be a safe no-op.
  recognizer_manager_handle_touch_event(&e, NULL);

  prv_destroy_recognizers(recognizers, k_rec_count);
}

void test_recognizer_manager__liftoff_nothing_completed_resets_to_wait_for_touchdown(void) {
  const int k_rec_count = 1;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window.layer, &layer_a);
  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);

  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_p(manager.active_layer, &layer_a);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersActive);
  prv_clear_recognizers_processed(&s_recognizers_handled);

  e.type = TouchEvent_PositionUpdate;
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);
  prv_clear_recognizers_processed(&s_recognizers_handled);

  // Liftoff with the recognizer still Possible: nothing completed the gesture, so the manager
  // must return to WaitForTouchdown instead of latching active_layer across strokes.
  e.type = TouchEvent_Liftoff;
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

void test_recognizer_manager__liftoff_completed_gesture_uses_existing_reset_path(void) {
  const int k_rec_count = 1;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window.layer, &layer_a);
  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);

  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  recognizer_manager_handle_touch_event(&e, &manager);
  prv_clear_recognizers_processed(&s_recognizers_handled);

  // The recognizer completes exactly at Liftoff: the pre-existing
  // prv_fail_then_reset_if_no_active_recognizers path must still fire the Completed
  // transition and reset the manager -- the new Liftoff branch must not short-circuit it
  // (it is a no-op here because the old path already reached WaitForTouchdown).
  e.type = TouchEvent_Liftoff;
  s_next_state = RecognizerState_Completed;
  s_idx_to_change = 0;
  recognizer_manager_handle_touch_event(&e, &manager);

  prv_compare_recognizers_processed((int[]){0}, 1, &s_recognizers_handled);
  prv_compare_recognizers_processed((int[]){0}, 1, &s_recognizers_reset);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

void test_recognizer_manager__appear_seam_set_before_cancel(void) {
  const int k_rec_count = 1;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window_a = {};
  layer_init(&window_a.layer, &ROOT_FRAME);
  Window window_b = {};
  layer_init(&window_b.layer, &ROOT_FRAME);

  RecognizerManager manager;
  recognizer_manager_init(&manager);
  s_manager = &manager;
  manager.window = &window_a;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window_a.layer, &layer_a);
  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);

  // A live stroke is triggered on window A.
  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  s_next_state = RecognizerState_Started;
  s_idx_to_change = 0;
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);

  // Window B appears: the glue must set the new window BEFORE cancelling, so the cancel walk
  // still reaches window A's live recognizer via the manager's (still-A) active_layer.
  app_window_recognizer_glue_window_focused(&window_b);

  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Cancelled);
  cl_assert_equal_p(manager.window, &window_b);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

void test_recognizer_manager__off_screen_choke_point_invariant(void) {
  const int k_rec_count = 1;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window_a = {};
  layer_init(&window_a.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  s_manager = &manager;
  manager.window = &window_a;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window_a.layer, &layer_a);
  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);

  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  s_next_state = RecognizerState_Started;
  s_idx_to_change = 0;
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_i(manager.state, RecognizerManagerState_RecognizersTriggered);

  app_window_recognizer_glue_window_off_screen(&window_a);

  cl_assert_equal_p(manager.window, NULL);
  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);

  // A window that isn't the manager's current window must be a no-op (guards against the
  // choke point double-resetting when called from the wrong site).
  app_window_recognizer_glue_window_off_screen(&window_a);
  cl_assert_equal_p(manager.window, NULL);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

void prv_focus_event_handler(PebbleEvent *e, void *context);

void test_recognizer_manager__focus_rows(void) {
  const int k_rec_count = 1;
  s_dummy_impl.handle_touch_event = prv_handle_touch_event_test;
  s_dummy_impl.reset = prv_reset_test;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window_a = {};
  layer_init(&window_a.layer, &ROOT_FRAME);
  RecognizerManager manager;
  recognizer_manager_init(&manager);
  s_manager = &manager;
  manager.window = &window_a;

  Layer layer_a;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_add_child(&window_a.layer, &layer_a);
  recognizer_add_to_list(recognizers[0], &layer_a.recognizer_list);

  TouchEvent e = {.type = TouchEvent_Touchdown};
  prv_set_touch_pos(&e, POINT_IN_A);
  s_next_state = RecognizerState_Started;
  s_idx_to_change = 0;
  recognizer_manager_handle_touch_event(&e, &manager);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Started);

  // Losing focus mid-stroke cancels the live recognizer but leaves window/state untouched.
  PebbleEvent will_lose_focus = {.app_focus = {.in_focus = false}};
  prv_focus_event_handler(&will_lose_focus, NULL);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Cancelled);
  cl_assert_equal_p(manager.window, &window_a);

  // Regaining focus repoints the manager at the top window and fully resets it.
  WindowStackItem top_item = {.window = &window_a};
  *app_state_get_window_stack() = (WindowStack){.list_head = &top_item.list_node};
  PebbleEvent will_gain_focus = {.app_focus = {.in_focus = true}};
  prv_focus_event_handler(&will_gain_focus, NULL);
  cl_assert_equal_p(manager.window, &window_a);
  cl_assert_equal_i(manager.state, RecognizerManagerState_WaitForTouchdown);
  cl_assert_equal_p(manager.active_layer, NULL);
  cl_assert_equal_p(manager.triggered, NULL);
  cl_assert_equal_i(recognizers[0]->state, RecognizerState_Possible);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

// Task 8A (impl plan section 13): single-owner contract for layer_attach_recognizer.
//
// Existing coverage (test_recognizer_manager__double_attach_does_not_double_count) already
// pins "attach an already-owned recognizer to a second layer is a silent no-op" as tested,
// intended behavior at the recognizer_add_to_list level (list membership never moves). This
// case extends that contract to the registration side: a second attach, resolved through a
// DIFFERENT RecognizerManager than the recognizer's true owner, must not re-parent
// recognizer->manager either. Before the fix, layer_attach_recognizer called
// recognizer_manager_register_recognizer unconditionally (before checking ownership), so a
// cross-layer/cross-manager attach would repoint recognizer->manager at the new manager while
// list membership (the real ownership root) silently stayed with the original layer --
// registration and list membership permanently diverge. Reusing s_manager (the app-task
// manager fixture) with two distinct RecognizerManager instances reproduces that divergence
// deterministically, standing in for the "different window/task" scenario the plan describes
// (unreachable in production today since window_get_recognizer_manager is per-task, not
// per-window, but a real code-level defect regardless).
void test_recognizer_manager__attach_while_owned_elsewhere_is_noop(void) {
  NEW_RECOGNIZER(r) = test_recognizer_create(&s_test_impl_data, NULL);

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);

  RecognizerManager manager_a, manager_b;
  recognizer_manager_init(&manager_a);
  recognizer_manager_init(&manager_b);
  manager_a.window = &window;
  manager_b.window = &window;

  Layer layer_a, layer_b;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_init(&layer_b, &LAYER_B_FRAME);
  layer_add_child(&window.layer, &layer_a);
  layer_add_child(&window.layer, &layer_b);

  s_manager = &manager_a;
  layer_attach_recognizer(&layer_a, r);
  cl_assert(recognizer_is_owned(r));
  cl_assert_equal_p(recognizer_get_manager(r), &manager_a);
  ListNode *layer_a_head_before = layer_a.recognizer_list.node;

  // A second attach to a different layer, resolved through a different manager, must be a
  // safe no-op: list ownership stays with layer A and the recognizer's registered manager
  // must not migrate to manager B.
  s_manager = &manager_b;
  layer_attach_recognizer(&layer_b, r);

  cl_assert(recognizer_is_owned(r));
  cl_assert_equal_p(recognizer_get_manager(r), &manager_a);
  cl_assert_equal_p(layer_a.recognizer_list.node, layer_a_head_before);
  cl_assert_equal_p(layer_b.recognizer_list.node, NULL);

  s_manager = &manager_a;
  layer_detach_recognizer(&layer_a, r);
}

// Task 8A (impl plan section 13): detaching from a non-owning layer must be a safe no-op
// that does not touch the true owner's list. Before the fix, recognizer_remove_from_list
// acted on whichever list->node was passed in regardless of actual membership, corrupting
// the true owner's head pointer when the wrong layer's detach was called.
void test_recognizer_manager__detach_from_non_owner_is_safe_noop(void) {
  const int k_rec_count = 2;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);

  Layer layer_a, layer_b;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_init(&layer_b, &LAYER_B_FRAME);
  layer_add_child(&window.layer, &layer_a);
  layer_add_child(&window.layer, &layer_b);

  // Two recognizers owned by layer A; recognizers[0] is the list head.
  layer_attach_recognizer(&layer_a, recognizers[0]);
  layer_attach_recognizer(&layer_a, recognizers[1]);
  cl_assert(recognizer_is_owned(recognizers[0]));
  cl_assert(recognizer_is_owned(recognizers[1]));

  // Layer B never owned recognizers[0]; detaching it from B must be a no-op.
  layer_detach_recognizer(&layer_b, recognizers[0]);

  // recognizers[0] must still be owned and layer A's list must still contain both
  // recognizers, in order, with an intact head.
  cl_assert(recognizer_is_owned(recognizers[0]));
  cl_assert(recognizer_is_owned(recognizers[1]));
  cl_assert_equal_p(layer_a.recognizer_list.node, &recognizers[0]->node);
  cl_assert_equal_p(list_get_next(&recognizers[0]->node), &recognizers[1]->node);
  cl_assert_equal_p(layer_b.recognizer_list.node, NULL);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  layer_detach_recognizer(&layer_a, recognizers[1]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

// Code review follow-up to the fix above: window_get_recognizer_manager is per-task, not
// per-window, so the realistic case has layer A and layer B resolving to the SAME manager.
// In that case recognizer_manager_deregister_recognizer's own manager-match check passes
// (they DO match), so before this fix the wrong-layer detach would still reset and orphan
// (manager = NULL) a recognizer that stays listed and dispatched on its true owning layer A.
// Setting s_manager once, shared by both layers, reproduces that realistic scenario.
void test_recognizer_manager__detach_from_non_owner_same_manager_is_safe_noop(void) {
  const int k_rec_count = 1;
  Recognizer **recognizers = prv_create_recognizers(k_rec_count);

  Window window = {};
  layer_init(&window.layer, &ROOT_FRAME);

  RecognizerManager manager;
  recognizer_manager_init(&manager);
  manager.window = &window;

  Layer layer_a, layer_b;
  layer_init(&layer_a, &LAYER_A_FRAME);
  layer_init(&layer_b, &LAYER_B_FRAME);
  layer_add_child(&window.layer, &layer_a);
  layer_add_child(&window.layer, &layer_b);

  // Both layers resolve to the same manager (the realistic per-task case).
  s_manager = &manager;

  layer_attach_recognizer(&layer_a, recognizers[0]);
  cl_assert(recognizer_is_owned(recognizers[0]));
  cl_assert_equal_p(recognizer_get_manager(recognizers[0]), &manager);

  // Layer B never owned recognizers[0]; detaching it from B must be a safe no-op, even
  // though layer B's manager lookup resolves to the SAME manager instance that actually
  // owns the recognizer.
  layer_detach_recognizer(&layer_b, recognizers[0]);

  // recognizers[0] must still be owned, still listed on layer A, and still registered with
  // the manager -- not reset/orphaned by the wrong-layer detach.
  cl_assert(recognizer_is_owned(recognizers[0]));
  cl_assert_equal_p(recognizer_get_manager(recognizers[0]), &manager);
  cl_assert_equal_p(layer_a.recognizer_list.node, &recognizers[0]->node);
  cl_assert_equal_p(layer_b.recognizer_list.node, NULL);

  layer_detach_recognizer(&layer_a, recognizers[0]);
  prv_destroy_recognizers(recognizers, k_rec_count);
}

// Series-level audit finding (impl plan section 12: "on 0->1" / "on 1->0"). The subscribe leg
// gated on new_count == 1, which is reachable from BOTH a genuine 0->1 transition and a 2->1
// transition (e.g. an app with two ScrollLayers destroying one). The cases below pin the
// intended edge-triggered behavior; test_recognizer_manager__attach_count_two_to_one_does_not_
// resubscribe is the one that reproduces the defect.
static void prv_other_touch_handler(const TouchEvent *event, void *context) {}

void test_recognizer_manager__attach_count_zero_to_one_subscribes(void) {
  cl_assert_equal_i(s_touch_subscribe_call_count, 0);

  app_window_recognizer_glue_attach_count_changed(1);  // 0 -> 1

  cl_assert_equal_i(s_touch_subscribe_call_count, 1);
  cl_assert(s_touch_subscribe_captured_handler != NULL);
  cl_assert_equal_i(s_touch_unsubscribe_call_count, 0);
}

void test_recognizer_manager__attach_count_one_to_two_does_not_resubscribe(void) {
  app_window_recognizer_glue_attach_count_changed(1);  // 0 -> 1
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);

  app_window_recognizer_glue_attach_count_changed(2);  // 1 -> 2

  cl_assert_equal_i(s_touch_subscribe_call_count, 1);
  cl_assert_equal_i(s_touch_unsubscribe_call_count, 0);
}

// The defect: a two-ScrollLayer app destroying one goes 2 -> 1, which must NOT be treated as
// a fresh 0 -> 1 attach. Before the fix this re-subscribes (a second touch_service_subscribe
// call), which would mid-session-reset the touch driver and re-clobber the raw_handler slot.
void test_recognizer_manager__attach_count_two_to_one_does_not_resubscribe(void) {
  app_window_recognizer_glue_attach_count_changed(1);  // 0 -> 1
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);
  app_window_recognizer_glue_attach_count_changed(2);  // 1 -> 2
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);

  app_window_recognizer_glue_attach_count_changed(1);  // 2 -> 1

  cl_assert_equal_i(s_touch_subscribe_call_count, 1);
  cl_assert_equal_i(s_touch_unsubscribe_call_count, 0);
}

void test_recognizer_manager__attach_count_one_to_zero_unsubscribes(void) {
  app_window_recognizer_glue_attach_count_changed(1);  // 0 -> 1
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);
  cl_assert(s_touch_subscribe_captured_handler != NULL);

  // The local touch_service_subscribe stub stands in for touch_service.c, so it doesn't
  // install the handler into raw_handler itself; simulate what the real implementation
  // would have done, so the unsubscribe leg's read-back guard sees a match.
  app_state_get_touch_service_state()->raw_handler = s_touch_subscribe_captured_handler;

  app_window_recognizer_glue_attach_count_changed(0);  // 1 -> 0

  cl_assert_equal_i(s_touch_unsubscribe_call_count, 1);
}

// The unsubscribe leg's existing read-back guard: an app that called touch_service_subscribe()
// itself after the glue now owns the single raw_handler slot, so the 1 -> 0 transition must not
// clobber it.
void test_recognizer_manager__attach_count_one_to_zero_guard_skips_foreign_handler(void) {
  app_window_recognizer_glue_attach_count_changed(1);  // 0 -> 1
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);

  app_state_get_touch_service_state()->raw_handler = prv_other_touch_handler;

  app_window_recognizer_glue_attach_count_changed(0);  // 1 -> 0

  cl_assert_equal_i(s_touch_unsubscribe_call_count, 0);
}
