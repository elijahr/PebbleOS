/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "scroll_layer.h"

#include "applib/applib_malloc.auto.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/content_indicator_private.h"
#include "applib/ui/shadows.h"
#include "applib/ui/window.h"
#include "process_management/app_manager.h"
#include "system/logging.h"
#include "system/passert.h"
#include "pbl/util/math.h"

#include "animation_timing.h"

#if CONFIG_TOUCH_WIDGET_DRAG
#include "applib/ui/recognizer/drag.h"
#include "services/touch/touch_click_suppress.h"
#endif

#include <string.h>

T_STATIC bool prv_scroll_layer_is_paging_enabled(ScrollLayer *scroll_layer) {
  PBL_ASSERTN(scroll_layer);
  if (process_manager_compiled_with_legacy2_sdk() || !scroll_layer->shadow_sublayer.hidden) {
    return false;
  }
  return !scroll_layer->paging.paging_disabled;
}

T_STATIC uint16_t prv_scroll_layer_get_paging_height(ScrollLayer *scroll_layer) {
  if (!prv_scroll_layer_is_paging_enabled(scroll_layer)) {
    return 0;
  }
  return MAX(0, scroll_layer->layer.frame.size.h);
}

//! Return callback_context or if NULL,
inline static void* get_callback_context(ScrollLayer *scroll_layer) {
  return scroll_layer->context ? scroll_layer->context : scroll_layer;
}

void scroll_layer_draw_shadow_sublayer(Layer *shadow_sublayer, GContext* ctx) {
  ScrollLayer *scroll_layer = (ScrollLayer *)(((uint8_t*)shadow_sublayer) - offsetof(ScrollLayer, shadow_sublayer));
  const GPoint content_offset = scroll_layer_get_content_offset(scroll_layer);
  const GSize content_size = scroll_layer_get_content_size(scroll_layer);
  const GSize frame_size = scroll_layer->layer.frame.size;
  GBitmap *shadow_top = shadow_get_top();
  GBitmap *shadow_bottom = shadow_get_bottom();

  graphics_context_set_compositing_mode(ctx, GCompOpClear);

  // Draw top shadow, if (partially) visible:
  const int16_t layer_w = shadow_sublayer->bounds.size.w;
  const int16_t layer_h = shadow_sublayer->bounds.size.h;
  const int16_t shadow_top_bitmap_h = shadow_top->bounds.size.h;
  const int16_t shadow_top_y_offset = -shadow_top_bitmap_h - CLIP(content_offset.y, -shadow_top_bitmap_h, 0);
  if (shadow_top_y_offset >= -shadow_top_bitmap_h) {
    graphics_draw_bitmap_in_rect(ctx, shadow_top, &GRect(0, shadow_top_y_offset,
                                                         layer_w, shadow_top_bitmap_h));
  }
  // Draw bottom shadow, if (partially) visible:
  const int16_t shadow_bottom_bitmap_h = shadow_top->bounds.size.h;
  const int16_t bottom_clipped_height = (content_size.h + content_offset.y) - frame_size.h;
  const int16_t shadow_bottom_y_offset = - CLIP(bottom_clipped_height, 0, shadow_bottom_bitmap_h);
  if (shadow_bottom_y_offset < 0) {
    graphics_draw_bitmap_in_rect(ctx, shadow_bottom, &GRect(0, layer_h + shadow_bottom_y_offset,
                                                            layer_w, shadow_bottom_bitmap_h));
  }
}

static void prv_setup_shadow_layer(ScrollLayer *scroll_layer) {
  layer_init(&scroll_layer->shadow_sublayer, &scroll_layer->layer.bounds);
  layer_set_clips(&scroll_layer->shadow_sublayer, true);
  scroll_layer->shadow_sublayer.update_proc = scroll_layer_draw_shadow_sublayer;
  layer_add_child(&scroll_layer->layer, &scroll_layer->shadow_sublayer);
}

#if CONFIG_TOUCH_WIDGET_DRAG
// Set for the exact duration of the drag-originated scroll_layer_set_content_offset() call below
// (see scroll_layer_is_dragging()). Not a ScrollLayer struct field: a new field trips the firmware
// build's exact-equality padding assert (src/fw/applib/applib_malloc.json; a bookkeeping fix via
// size_3x_padding, not an ABI wall) -- skipped since this is transient, single-drag-in-flight state
// that shouldn't grow the app-facing allocation budget. A single module-level pointer is correct
// here because touch delivery is single-threaded and serialized: at most one drag is ever in
// flight at a time.
static ScrollLayer *s_dragging_scroll_layer;

bool scroll_layer_is_dragging(const ScrollLayer *scroll_layer) {
  return scroll_layer && (scroll_layer == s_dragging_scroll_layer);
}

static void prv_drag_event_cb(const Recognizer *recognizer, RecognizerEvent event) {
  ScrollLayer *scroll_layer = recognizer_get_user_data(recognizer);
  switch (event) {
    case RecognizerEvent_Started:
    case RecognizerEvent_Updated: {
      const int16_t dy = drag_recognizer_get_delta_y(recognizer);
      const GPoint before = scroll_layer_get_content_offset(scroll_layer);
      // Mark this specific, synchronous offset update as drag-originated so
      // .content_offset_changed_handler (e.g. MenuLayer's selection reconciliation) can tell it
      // apart from any other caller. Scoped tightly around the call: cleared before returning,
      // never left set across event boundaries.
      s_dragging_scroll_layer = scroll_layer;
      scroll_layer_set_content_offset(scroll_layer, GPoint(before.x, before.y + dy),
                                      false /* unanimated; unschedules any stale animation */);
      s_dragging_scroll_layer = NULL;
      const GPoint after = scroll_layer_get_content_offset(scroll_layer);
      if (after.y != before.y) {
        touch_click_suppress_mark_consumed();
      }
      break;
    }
    case RecognizerEvent_Completed:
    case RecognizerEvent_Cancelled:
      // No snap-back in stage 1.
      break;
  }
}
#endif

static void scroll_layer_property_changed_proc(Layer *layer) {
  ScrollLayer *scroll_layer = (ScrollLayer*)layer;
  const GRect internal_rect = (GRect) { GPointZero, scroll_layer->layer.frame.size };

  // If shadow_sublayer initialized (opposite of paging_enabled)
  if (!prv_scroll_layer_is_paging_enabled(scroll_layer)) {
    scroll_layer->shadow_sublayer.frame = internal_rect;
    scroll_layer->shadow_sublayer.bounds = internal_rect;
  }

  layer_set_frame(&scroll_layer->content_sublayer, &internal_rect);
}

void scroll_layer_init(ScrollLayer *scroll_layer, const GRect *frame) {
  *scroll_layer = (ScrollLayer){};

  layer_init(&scroll_layer->layer, frame);
  const GRect *bounds = &scroll_layer->layer.bounds;
  scroll_layer->layer.property_changed_proc = scroll_layer_property_changed_proc;

  layer_init(&scroll_layer->content_sublayer, bounds);
  layer_add_child(&scroll_layer->layer, &scroll_layer->content_sublayer);

  prv_setup_shadow_layer(scroll_layer);

#if CONFIG_TOUCH_WIDGET_DRAG
  // Owned by scroll_layer->layer.recognizer_list; reclaimed by layer_deinit
  // (scroll_layer_deinit -> layer_deinit). NULL from heap exhaustion flows
  // into layer_attach_recognizer's own NULL-recognizer no-op: the widget
  // silently lacks drag, fail-open.
  Recognizer *drag_recognizer = drag_recognizer_create(prv_drag_event_cb, scroll_layer);
  layer_attach_recognizer(&scroll_layer->layer, drag_recognizer);
#endif
}

ScrollLayer* scroll_layer_create(GRect frame) {
  ScrollLayer *layer = applib_type_malloc(ScrollLayer);
  if (layer) {
    scroll_layer_init(layer, &frame);
  }
  return layer;
}

bool scroll_layer_is_instance(const Layer *layer) {
  return layer && layer->property_changed_proc == scroll_layer_property_changed_proc;
}

void scroll_layer_deinit(ScrollLayer *scroll_layer) {
  animation_destroy(property_animation_get_animation(scroll_layer->animation));
  content_indicator_destroy_for_scroll_layer(scroll_layer);
  layer_deinit(&scroll_layer->layer);
}

void scroll_layer_destroy(ScrollLayer *scroll_layer) {
  if (scroll_layer == NULL) {
    return;
  }
  scroll_layer_deinit(scroll_layer);
  applib_free(scroll_layer);
}

Layer* scroll_layer_get_layer(const ScrollLayer *scroll_layer) {
  return &((ScrollLayer *)scroll_layer)->layer;
}

void scroll_layer_set_frame(ScrollLayer *scroll_layer, GRect rect) {
  scroll_layer->layer.frame = rect;
  layer_mark_dirty(&scroll_layer->layer);
}

void scroll_layer_add_child(ScrollLayer *scroll_layer, Layer *child) {
  layer_add_child(&scroll_layer->content_sublayer, child);
}


GPoint scroll_layer_get_content_offset(ScrollLayer *scroll_layer) {
  return scroll_layer->content_sublayer.bounds.origin;
}

T_STATIC void prv_scroll_layer_set_content_offset_internal(
    ScrollLayer *scroll_layer, GPoint offset) {
  const GSize frame_size = scroll_layer->layer.frame.size;
  GRect bounds = scroll_layer->content_sublayer.bounds;
  const GPoint old_offset = bounds.origin;
  const int16_t min_x_offset = frame_size.w - bounds.size.w;
  int16_t min_y_offset = frame_size.h - bounds.size.h;

  if (prv_scroll_layer_is_paging_enabled(scroll_layer)) {
    uint16_t page_height = prv_scroll_layer_get_paging_height(scroll_layer);
    if (page_height) {
      // showing full page-aligned contents of last page
      min_y_offset = ROUND_TO_MOD_CEIL(min_y_offset, page_height);
    }
  }

  if (scroll_layer_get_clips_content_offset(scroll_layer)) {
    bounds.origin.x = CLIP(offset.x, MIN(min_x_offset, 0), 0);
    bounds.origin.y = CLIP(offset.y, MIN(min_y_offset, 0), 0);
  } else {
    bounds.origin = offset;
  }

  if (gpoint_equal(&old_offset, &bounds.origin)) {
    // Not changed.
    // still, call update_content indicator to refresh potential timers
    scroll_layer_update_content_indicator(scroll_layer);

    return;
  }

  layer_set_bounds(&scroll_layer->content_sublayer, &bounds);
  scroll_layer_update_content_indicator(scroll_layer);

  if (scroll_layer->callbacks.content_offset_changed_handler) {
    scroll_layer->callbacks.content_offset_changed_handler(scroll_layer, get_callback_context(scroll_layer));
  }
}

void scroll_layer_set_content_offset(ScrollLayer *scroll_layer, GPoint offset, bool animated) {
  // Note: animation_is_scheduled() returns false and property_animation_destroy does nothing
  // if the argument is NULL
  Animation *animation = property_animation_get_animation(scroll_layer->animation);
  bool was_running = false;
  if (animation) {
    was_running = animation_is_scheduled(animation);
    if (was_running) {
      animation_unschedule(animation);
    }
  }
  if (animated) {
    static const PropertyAnimationImplementation implementation = {
      .base = {
        .update = (AnimationUpdateImplementation) property_animation_update_gpoint,
      },
      .accessors = {
        .setter = { .grect = (const GRectSetter) (void *) prv_scroll_layer_set_content_offset_internal, },
        .getter = { .grect = (const GRectGetter) (void *) scroll_layer_get_content_offset, },
      },
    };
    if (animation) {
      property_animation_init(scroll_layer->animation, &implementation, scroll_layer, NULL,
                              &offset);
      if (was_running && !scroll_layer_get_paging(scroll_layer)) {
        animation_set_curve(animation, AnimationCurveEaseOut);
      }
    } else {
      scroll_layer->animation = property_animation_create(&implementation, scroll_layer, NULL,
                                                          &offset);
      animation = property_animation_get_animation(scroll_layer->animation);
      if (scroll_layer_get_paging(scroll_layer)) {
        animation_set_custom_interpolation(animation, interpolate_moook);
        animation_set_duration(animation, interpolate_moook_duration());
      }
      animation_set_auto_destroy(animation, false);
    }
    animation_schedule(animation);
  } else {
    prv_scroll_layer_set_content_offset_internal(scroll_layer, offset);
  }
}

void scroll_layer_set_content_size(ScrollLayer *scroll_layer, GSize size) {
  GRect bounds = scroll_layer->content_sublayer.bounds;
  bounds.size = size;
  layer_set_bounds(&scroll_layer->content_sublayer, &bounds);
  // Ensure our content offset is clipped to the new size.
  // We call prv_scroll_layer_set_content_offset_internal() directly and
  // keep potential animations running – since some 3rd-party apps do change the content size
  // frequently (e.g. in an update_proc) and would otherwise implicitly stop scroll animations.
  // It's fine to keep scroll animations running as they clip the offset to valid bounds.
  prv_scroll_layer_set_content_offset_internal(
      scroll_layer, scroll_layer_get_content_offset(scroll_layer));
}

GSize scroll_layer_get_content_size(const ScrollLayer *scroll_layer) {
  return scroll_layer->content_sublayer.bounds.size;
}

void scroll_layer_scroll(ScrollLayer *scroll_layer, ScrollDirection direction, bool animated) {
  GPoint offset = scroll_layer_get_content_offset(scroll_layer);
  int32_t scroll_height = 32;

  // If process is 3.x and has enabled paging
  if (prv_scroll_layer_is_paging_enabled(scroll_layer)) {
    uint16_t page_height = prv_scroll_layer_get_paging_height(scroll_layer);
    if (page_height) {
      // Force offset to start (and stay) page aligned
      offset.y = ROUND_TO_MOD_CEIL(offset.y, page_height);
      scroll_height = page_height;
    }
  }

  switch (direction) {
    case ScrollDirectionUp:
      offset.y += scroll_height;
      break;
    case ScrollDirectionDown:
      offset.y -= scroll_height;
      break;

    default: return;
  }
  scroll_layer_set_content_offset(scroll_layer, offset, animated);
}

void scroll_layer_scroll_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  ScrollLayer *scroll_layer = (ScrollLayer *)context;
  scroll_layer_scroll(scroll_layer, ScrollDirectionUp, true);
  (void)recognizer;
}

void scroll_layer_scroll_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  ScrollLayer *scroll_layer = (ScrollLayer *)context;
  scroll_layer_scroll(scroll_layer, ScrollDirectionDown, true);
  (void)recognizer;
}

static void scroll_layer_click_config_provider(ScrollLayer *scroll_layer) {
  // Config UP / DOWN button behavior:
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, scroll_layer_scroll_up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, scroll_layer_scroll_down_click_handler);
  // Set the context for the SELECT button:
  window_set_click_context(BUTTON_ID_SELECT, get_callback_context(scroll_layer));

  // Callback to provide the client to setup the SELECT button:
  if (scroll_layer->callbacks.click_config_provider) {
    scroll_layer->callbacks.click_config_provider(get_callback_context(scroll_layer));
  }
}

void scroll_layer_set_click_config_onto_window(ScrollLayer *scroll_layer, struct Window *window) {
  window_set_click_config_provider_with_context(window, (ClickConfigProvider) scroll_layer_click_config_provider, scroll_layer);
}

void scroll_layer_set_callbacks(ScrollLayer *scroll_layer, ScrollLayerCallbacks callbacks) {
  scroll_layer->callbacks = callbacks;
}

void scroll_layer_set_context(ScrollLayer *scroll_layer, void *context) {
  scroll_layer->context = context;
}

void scroll_layer_set_shadow_hidden(ScrollLayer *scroll_layer, bool hidden) {
  PBL_ASSERTN(scroll_layer);

  // paging and shadow_sublayer are mutually exclusive
  // so init shadow_sublayer if it was paging data
  if (prv_scroll_layer_is_paging_enabled(scroll_layer) && hidden == false) {
    prv_setup_shadow_layer(scroll_layer);
  }

  scroll_layer_property_changed_proc((Layer*)scroll_layer);
  layer_set_hidden(&scroll_layer->shadow_sublayer, hidden);
}

bool scroll_layer_get_shadow_hidden(const ScrollLayer *scroll_layer) {
  return layer_get_hidden(&scroll_layer->shadow_sublayer);
}

void scroll_layer_set_paging(ScrollLayer *scroll_layer, bool paging_enabled) {
  PBL_ASSERTN(scroll_layer);
  if (paging_enabled) {
    // Deinit shadow_sublayer to enable paging
    if (!prv_scroll_layer_is_paging_enabled(scroll_layer)) {
      layer_deinit(&scroll_layer->shadow_sublayer);
    }

    // paging and shadow_sublayer are mutually exclusive
    scroll_layer->paging.shadow_hidden = true;
    scroll_layer->paging.paging_disabled = false;
  } else {
    if (prv_scroll_layer_is_paging_enabled(scroll_layer)) {
      prv_setup_shadow_layer(scroll_layer);
      // still require explicit un-hiding of shadow
      scroll_layer_set_shadow_hidden(scroll_layer, true);
    }
  }
}

bool scroll_layer_get_paging(ScrollLayer* scroll_layer) {
  return scroll_layer && prv_scroll_layer_is_paging_enabled(scroll_layer);
}

ContentIndicator *scroll_layer_get_content_indicator(ScrollLayer *scroll_layer) {
  return content_indicator_get_or_create_for_scroll_layer(scroll_layer);
}

void scroll_layer_update_content_indicator(ScrollLayer *scroll_layer) {
  ContentIndicator *content_indicator = content_indicator_get_for_scroll_layer(scroll_layer);
  if (!content_indicator) {
    return;
  }

  const GSize scroll_layer_frame_size = scroll_layer_get_layer(scroll_layer)->frame.size;
  const GSize scroll_layer_content_size = scroll_layer_get_content_size(scroll_layer);
  const int16_t scroll_layer_content_offset_y = scroll_layer_get_content_offset(scroll_layer).y;

  const bool content_available_up = (scroll_layer_content_offset_y < 0);
  content_indicator_set_content_available(content_indicator,
                                          ContentIndicatorDirectionUp,
                                          content_available_up);
  const bool content_available_down =
    (scroll_layer_frame_size.h - scroll_layer_content_offset_y < scroll_layer_content_size.h);
  content_indicator_set_content_available(content_indicator,
                                          ContentIndicatorDirectionDown,
                                          content_available_down);
}

void scroll_layer_set_clips_content_offset(ScrollLayer *scroll_layer, bool clips) {
  scroll_layer->content_sublayer.clips = clips;
  scroll_layer_set_content_offset(scroll_layer,
                                  scroll_layer_get_content_offset(scroll_layer), false);
}

bool scroll_layer_get_clips_content_offset(ScrollLayer *scroll_layer) {
  return scroll_layer->content_sublayer.clips;
}
