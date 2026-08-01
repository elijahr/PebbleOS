/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// End-to-end coverage for the production touch-subscription call chain:
//
//   layer_attach_recognizer  ->  app_state_recognizer_attach_count_inc
//                            ->  app_window_recognizer_glue_attach_count_changed
//                            ->  touch_service_subscribe
//
// and the symmetric detach/dec/unsubscribe path. Every other suite either stubs
// app_state_recognizer_attach_count_inc/dec (tests/stubs/stubs_app_state.h) or hand-feeds
// counts straight to the glue, so severing either link left the whole suite green. This is
// the only suite that compiles the REAL src/fw/process_state/app_state/app_state.c, which
// is what makes both links mutation-detectable here.

#include "clar.h"

#include "applib/touch_service.h"
#include "applib/ui/app_window_recognizer_glue.h"
#include "applib/ui/layer.h"
#include "applib/ui/recognizer/recognizer.h"
#include "applib/ui/recognizer/recognizer_impl.h"
#include "applib/ui/recognizer/recognizer_list.h"
#include "applib/ui/recognizer/recognizer_manager.h"
#include "applib/ui/recognizer/recognizer_private.h"
#include "kernel/util/segment.h"
#include "process_state/app_state/app_state.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

// Stubs
#include "stubs_analytics.h"
#include "stubs_app_install_manager.h"
#include "stubs_click.h"
#include "stubs_event_service_client.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_process_manager.h"
#include "stubs_prompt.h"
#include "stubs_queue.h"
#include "stubs_resources.h"
#include "stubs_syscalls.h"

#include "fake_pebble_tasks.h"

#include "test_recognizer_impl.h"

// ---------------------------------------------------------------------------
// touch_service.c is not compiled here. These fakes count calls and mirror the real
// service's one observable side effect the glue depends on: raw_handler ownership, which
// the 1->0 branch checks before unsubscribing.
// ---------------------------------------------------------------------------
static int s_touch_subscribe_call_count;
static int s_touch_unsubscribe_call_count;

void touch_service_subscribe(TouchServiceHandler handler, void *context) {
  s_touch_subscribe_call_count++;
  app_state_get_touch_service_state()->raw_handler = handler;
}

void touch_service_unsubscribe(void) {
  s_touch_unsubscribe_call_count++;
  app_state_get_touch_service_state()->raw_handler = NULL;
}

// The attach path resolves a manager through the layer's window. A NULL manager keeps this
// suite's link closure to layer.c + the recognizer core: the counter increments on
// ownership transitions regardless of manager presence.
RecognizerManager *window_get_recognizer_manager(struct Window *window) {
  return NULL;
}

// ---------------------------------------------------------------------------
// Link closure for the real app_state.c. Everything below is referenced only from
// app_state_init/app_state_deinit (which this suite never calls -- it calls
// app_state_configure and drives the attach counter directly) or from layer.c paths that are
// not under test here. Keeping them as local no-ops holds the suite to the four translation
// units that actually make up the chain.
// ---------------------------------------------------------------------------
void animation_private_state_init(AnimationState *state) {}
void animation_private_state_deinit(AnimationState *state) {}
void app_manager_get_framebuffer_size(GSize *size) { *size = GSize(0, 0); }
void app_message_init(void) {}
void app_outbox_init(void) {}
void ble_init_app_state(void) {}
void accel_service_state_init(AccelServiceState *state) {}
void plugin_service_state_init(PluginServiceState *state) {}
void battery_state_service_state_init(BatteryStateServiceState *state) {}
void backlight_service_state_init(BacklightServiceState *state) {}
void connection_service_state_init(ConnectionServiceState *state) {}
void tick_timer_service_state_init(TickTimerServiceState *state) {}
void touch_service_state_init(TouchServiceState *state) {}
void health_service_state_init(HealthServiceState *state) {}
void health_service_state_deinit(HealthServiceState *state) {}
void locale_init_app_locale(LocaleInfo *info) {}
void content_indicator_init_buffer(ContentIndicatorsBuffer *buffer) {}
void app_glance_service_init_glance(AppGlance *glance) {}
void click_manager_init(ClickManager *click_manager) {}
void framebuffer_init(FrameBuffer *fb, const GSize *size) {}
void framebuffer_clear(FrameBuffer *f) {}
void graphics_context_init(GContext *ctx, FrameBuffer *framebuffer,
                           GContextInitializationMode init_mode) {}
bool graphics_release_frame_buffer(GContext *ctx, GBitmap *buffer) { return true; }
void unobstructed_area_service_init(UnobstructedAreaState *state, int16_t current_y) {}
void unobstructed_area_service_deinit(UnobstructedAreaState *state) {}
void unobstructed_area_service_get_area(UnobstructedAreaState *state, GRect *area) {
  *area = GRect(0, 0, 0, 0);
}
uint16_t gbitmap_format_get_row_size_bytes(int16_t width, GBitmapFormat format) { return 0; }
void window_schedule_render(Window *window) {}
Window *app_window_stack_get_top_window(void) { return NULL; }

static Layer *s_layer_tree_stack[LAYER_TREE_STACK_SIZE];
Layer **kernel_applib_get_layer_tree_stack(void) { return s_layer_tree_stack; }

// The manager is NULL throughout this suite (window_get_recognizer_manager above), so none of
// these run on the attach/detach path. recognizer_manager.c is deliberately not linked: its
// behavior is covered by test_recognizer_manager, and linking it would pull in the whole
// window/window_stack/modal_manager closure for no added coverage of the chain under test.
void recognizer_manager_init(RecognizerManager *manager) {}
void recognizer_manager_handle_touch_event(const TouchEvent *touch_event, void *context) {}
void recognizer_manager_set_window(RecognizerManager *manager, struct Window *window) {}
void recognizer_manager_cancel_touches(RecognizerManager *manager) {}
void recognizer_manager_reset(RecognizerManager *manager) {}
void recognizer_manager_register_recognizer(RecognizerManager *manager, Recognizer *recognizer) {}
void recognizer_manager_deregister_recognizer(RecognizerManager *manager, Recognizer *recognizer) {}
void recognizer_manager_handle_state_change(RecognizerManager *manager, Recognizer *changed) {}

// ---------------------------------------------------------------------------
// AppState backing store. app_state_configure() carves the real AppState out of this.
// ---------------------------------------------------------------------------
// Oversized on purpose: AppState's size is private to app_state.c, and app_state_configure
// fails rather than overruns if the segment is too small.
static alignas(max_align_t) uint8_t s_app_state_ram[256 * 1024];

static Layer s_layer;
static Recognizer *s_recognizer;
static TestImplData s_test_impl_data;

void test_app_state_recognizer_chain__initialize(void) {
  s_touch_subscribe_call_count = 0;
  s_touch_unsubscribe_call_count = 0;

  memset(s_app_state_ram, 0, sizeof(s_app_state_ram));
  MemorySegment app_state_ram = {
    .start = s_app_state_ram,
    .end = s_app_state_ram + sizeof(s_app_state_ram),
  };
  cl_assert(app_state_configure(&app_state_ram, ProcessAppSDKType_4x, 0));

  stub_pebble_tasks_set_current(PebbleTask_App);

  layer_init(&s_layer, &GRect(0, 0, 100, 100));
  s_test_impl_data = (TestImplData) {};
  s_recognizer = test_recognizer_create(&s_test_impl_data, NULL);
  cl_assert(s_recognizer != NULL);
}

void test_app_state_recognizer_chain__cleanup(void) {
  recognizer_destroy(s_recognizer);
  s_recognizer = NULL;
  stub_pebble_tasks_set_current(PebbleTask_KernelMain);
}

// layer_attach_recognizer -> app_state inc -> glue -> touch_service_subscribe.
void test_app_state_recognizer_chain__attach_subscribes_touch_service(void) {
  cl_assert_equal_i(s_touch_subscribe_call_count, 0);

  layer_attach_recognizer(&s_layer, s_recognizer);

  cl_assert_equal_i(app_state_recognizer_attach_count(), 1);
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);
  cl_assert_equal_i(s_touch_unsubscribe_call_count, 0);
  cl_assert(app_state_get_touch_service_state()->raw_handler != NULL);
}

// layer_detach_recognizer -> app_state dec -> glue -> touch_service_unsubscribe.
void test_app_state_recognizer_chain__detach_unsubscribes_touch_service(void) {
  layer_attach_recognizer(&s_layer, s_recognizer);
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);

  layer_detach_recognizer(&s_layer, s_recognizer);

  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);
  cl_assert_equal_i(s_touch_subscribe_call_count, 1);
  cl_assert_equal_i(s_touch_unsubscribe_call_count, 1);
  cl_assert(app_state_get_touch_service_state()->raw_handler == NULL);
}
