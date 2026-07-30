/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/ui/layer.h"
#include "applib/ui/layer_private.h"
#include "pbl/util/size.h"

#include "clar.h"

// Stubs
////////////////////////////////////
#include "stubs_app_state.h"
#include "stubs_bitblt.h"
#include "stubs_compiled_with_legacy2_sdk.h"
#include "stubs_gbitmap.h"
#include "stubs_heap.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_pebble_tasks.h"
#include "stubs_resources.h"
#include "stubs_syscalls.h"
#include "stubs_unobstructed_area.h"

// Setup
////////////////////////////////////

void test_layer__initialize(void) {
}

void test_layer__cleanup(void) {
}

GDrawState graphics_context_get_drawing_state(GContext *ctx) {
  return (GDrawState) { 0 };
}

bool graphics_release_frame_buffer(GContext *ctx, GBitmap *buffer) {
  return false;
}

void graphics_context_set_drawing_state(GContext *ctx, GDrawState draw_state) {
}

void window_schedule_render(struct Window *window) {
}

void recognizer_destroy(Recognizer *recognizer) {}

// Minimal functional list stubs so layer_deinit exercises its detach-and-destroy iteration.
// These diverge from the real recognizer list on purpose; do not "fix" them and do not
// write tests that rely on the extra permissiveness:
// - recognizer_list_iterate snapshots the whole list up front, while the real one only
//   captures each node's next pointer before its callback: a callback that removes a
//   not-yet-visited entry passes here but would touch a stale node in real firmware
// - the list argument is ignored; one global array backs every list
// - recognizer_add_to_list has no is_owned guard against double-add
// - recognizer_remove_from_list removes by swap-with-last, reordering the remaining
//   entries, while the real list_remove unlinks in place and preserves order
// (the array also caps at MAX_TEST_RECOGNIZERS entries -- a harness limit, not a
// semantic divergence)
#define MAX_TEST_RECOGNIZERS 4
static Recognizer *s_attached_recognizers[MAX_TEST_RECOGNIZERS];
static int s_num_attached_recognizers;

void recognizer_add_to_list(Recognizer *recognizer, RecognizerList *list) {
  cl_assert(s_num_attached_recognizers < MAX_TEST_RECOGNIZERS);
  s_attached_recognizers[s_num_attached_recognizers++] = recognizer;
}

void recognizer_remove_from_list(Recognizer *recognizer, RecognizerList *list) {
  for (int i = 0; i < s_num_attached_recognizers; ++i) {
    if (s_attached_recognizers[i] == recognizer) {
      s_attached_recognizers[i] = s_attached_recognizers[--s_num_attached_recognizers];
      return;
    }
  }
}

// Ownership is modeled as membership in the global array above
bool recognizer_is_owned(Recognizer *recognizer) {
  for (int i = 0; i < s_num_attached_recognizers; ++i) {
    if (s_attached_recognizers[i] == recognizer) {
      return true;
    }
  }
  return false;
}

// The list argument is ignored (see comment above); membership in the global array is the
// only list this stub knows about.
bool recognizer_is_in_list(Recognizer *recognizer, RecognizerList *list) {
  return recognizer_is_owned(recognizer);
}

RecognizerManager *window_get_recognizer_manager(Window *window) { return NULL; }

bool recognizer_list_iterate(RecognizerList *list, RecognizerListIteratorCb iter_cb,
                             void *context) {
  // Iterate a snapshot: the callback may remove entries mid-iteration
  Recognizer *snapshot[MAX_TEST_RECOGNIZERS];
  const int num = s_num_attached_recognizers;
  for (int i = 0; i < num; ++i) {
    snapshot[i] = s_attached_recognizers[i];
  }
  for (int i = 0; i < num; ++i) {
    if (!iter_cb(snapshot[i], context)) {
      return false;
    }
  }
  return true;
}

void recognizer_manager_register_recognizer(RecognizerManager *manager, Recognizer *recognizer) {}

void recognizer_manager_deregister_recognizer(RecognizerManager *manager, Recognizer *recognizer) {}
// Tests
////////////////////////////////////

void test_layer__add_child_and_remove_from_parent(void) {
  Layer parent, child_a, child_b, child_c, grand_child_a;
  Layer *layers[] = {&parent, &child_a, &child_b, &child_c, &grand_child_a};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  // Create this hierarchy:
  // This hits first_child and next_sibling add code paths.
  //
  // +-parent
  //     |
  //     '->child_a->child_b->child_c
  //           |
  //           '->grand_child_a
  //
  cl_assert(parent.first_child == NULL);
  layer_add_child(&parent, &child_a);
  cl_assert(parent.first_child == &child_a);
  layer_add_child(&parent, &child_b);
  cl_assert(parent.first_child == &child_a);
  cl_assert(child_a.next_sibling == &child_b);
  cl_assert(child_a.parent == &parent);
  cl_assert(child_b.parent == &parent);
  layer_add_child(&parent, &child_c);
  cl_assert(child_c.parent == &parent);
  cl_assert(child_b.next_sibling == &child_c);
  layer_add_child(&child_a, &grand_child_a);
  cl_assert(grand_child_a.parent == &child_a);

  // Remove non-first-child (child_b):
  //
  // +-parent
  //     |
  //     '->child_a->child_c
  //           |
  //           '->grand_child_a
  //
  // +-child_b
  //
  layer_remove_from_parent(&child_b);
  cl_assert(child_b.parent == NULL);
  cl_assert(child_b.next_sibling == NULL);
  cl_assert(parent.first_child == &child_a);
  cl_assert(child_a.next_sibling == &child_c);
  cl_assert(grand_child_a.parent == &child_a);
  cl_assert(child_c.parent == &parent);

  // Remove first-child (child_a):
  //
  // +-parent
  //     |
  //     '->child_c
  //
  // +-child_a
  //       |
  //       '->grand_child_a
  //
  layer_remove_from_parent(&child_a);
  cl_assert(parent.first_child == &child_c);
  cl_assert(child_c.parent == &parent);
  cl_assert(child_a.parent == NULL);
  cl_assert(child_a.next_sibling == NULL);
  cl_assert(grand_child_a.parent == &child_a);

  // Return early when (parent->paren == NULL):
  layer_remove_from_parent(&parent);
}

void test_layer__remove_child_layers(void) {
  Layer parent, child_a, child_b;
  Layer *layers[] = {&parent, &child_a, &child_b};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  // Create this hierarchy:
  //
  // +-parent
  //     |
  //     '->child_a->child_b
  //
  layer_add_child(&parent, &child_a);
  layer_add_child(&parent, &child_b);
  layer_remove_child_layers(&parent);
  cl_assert(child_a.parent == NULL);
  cl_assert(child_a.next_sibling == NULL);
  cl_assert(child_b.parent == NULL);
  cl_assert(parent.first_child == NULL);
}

void test_layer__insert_below(void) {
  Layer parent, child_a, child_b, child_c;
  Layer *layers[] = {&parent, &child_a, &child_b, &child_c};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  // Create this hierarchy:
  //
  // +-parent
  //     |
  //     '->child_a
  //
  layer_add_child(&parent, &child_a);

  // Insert child_b below child_b (first_child code path):
  //
  // +-parent
  //     |
  //     '->child_b->child_a
  //
  layer_insert_below_sibling(&child_b, &child_a);
  cl_assert(child_b.parent == &parent);
  cl_assert(child_b.next_sibling == &child_a);
  cl_assert(child_a.next_sibling == NULL);

  // Insert child_c below child_a (next_sibling code path):
  //
  // +-parent
  //     |
  //     '->child_b->child_c->child_a
  //
  layer_insert_below_sibling(&child_c, &child_a);
  cl_assert(parent.first_child == &child_b);
  cl_assert(child_b.next_sibling == &child_c);
  cl_assert(child_c.parent == &parent);
  cl_assert(child_c.next_sibling == &child_a);
  cl_assert(child_a.next_sibling == NULL);
}

void test_layer__insert_above(void) {
  Layer parent, child_a, child_b, child_c;
  Layer *layers[] = {&parent, &child_a, &child_b, &child_c};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  // Create this hierarchy:
  //
  // +-parent
  //     |
  //     '->child_b
  //
  layer_add_child(&parent, &child_b);

  // Insert child_a above child_b (first_child code path):
  //
  // +-parent
  //     |
  //     '->child_b->child_a
  //
  layer_insert_above_sibling(&child_a, &child_b);
  cl_assert(child_b.parent == &parent);
  cl_assert(child_b.next_sibling == &child_a);
  cl_assert(child_a.next_sibling == NULL);

  // Insert child_c above child_b (next_sibling code path):
  //
  // +-parent
  //     |
  //     '->child_b->child_c->child_a
  //
  layer_insert_above_sibling(&child_c, &child_b);
  cl_assert(parent.first_child == &child_b);
  cl_assert(child_b.next_sibling == &child_c);
  cl_assert(child_c.parent == &parent);
  cl_assert(child_c.next_sibling == &child_a);
  cl_assert(child_a.next_sibling == NULL);
}

void test_layer__traverse(void) {
  Layer *stack[5];
  uint8_t current_stack = 0;

  Layer *a = layer_create(GRectZero);
  Layer *aa = layer_create(GRectZero);
  Layer *aaa = layer_create(GRectZero);
  Layer *aaaa = layer_create(GRectZero);
  Layer *ab = layer_create(GRectZero);
  Layer *b = layer_create(GRectZero);

  layer_add_child(a, aa);
  layer_add_child(aa, aaa);
  layer_add_child(aaa, aaaa);
  layer_add_child(a, ab);
  a->next_sibling = b;

  stack[0] = a;

  // go to child if possible
  Layer *actual = __layer_tree_traverse_next__test_accessor(stack, ARRAY_LENGTH(stack), &current_stack,
      true);
  cl_assert_equal_p(aa, actual);
  cl_assert_equal_i(1, current_stack);
  cl_assert_equal_p(a, stack[0]);
  cl_assert_equal_p(aa, stack[1]);

  // go to child if possible
  actual = __layer_tree_traverse_next__test_accessor(stack, ARRAY_LENGTH(stack), &current_stack, true);
  cl_assert_equal_p(aaa, actual);
  cl_assert_equal_i(2, current_stack);
  cl_assert_equal_p(a, stack[0]);
  cl_assert_equal_p(aa, stack[1]);
  cl_assert_equal_p(aaa, stack[2]);

  // go to child if possible
  actual = __layer_tree_traverse_next__test_accessor(stack, ARRAY_LENGTH(stack), &current_stack, true);
  cl_assert_equal_p(aaaa, actual);
  cl_assert_equal_i(3, current_stack);
  cl_assert_equal_p(a, stack[0]);
  cl_assert_equal_p(aa, stack[1]);
  cl_assert_equal_p(aaa, stack[2]);
  cl_assert_equal_p(aaaa, stack[3]);

  // go back two levels and then to sibling
  actual = __layer_tree_traverse_next__test_accessor(stack, ARRAY_LENGTH(stack), &current_stack, true);
  cl_assert_equal_p(ab, actual);
  cl_assert_equal_i(1, current_stack);
  cl_assert_equal_p(a, stack[0]);
  cl_assert_equal_p(ab, stack[1]);

  // go back one level and then to sibling
  actual = __layer_tree_traverse_next__test_accessor(stack, ARRAY_LENGTH(stack), &current_stack, true);
  cl_assert_equal_p(b, actual);
  cl_assert_equal_i(0, current_stack);
  cl_assert_equal_p(b, stack[0]);

  // no more siblings on root level
  actual = __layer_tree_traverse_next__test_accessor(stack, ARRAY_LENGTH(stack), &current_stack, true);
  cl_assert_equal_p(NULL, actual);
  cl_assert_equal_i(0, current_stack);

  // do not descend
  stack[0] = a;
  current_stack = 0;
  actual = __layer_tree_traverse_next__test_accessor(stack, ARRAY_LENGTH(stack), &current_stack, false);
  cl_assert_equal_p(b, actual);
  cl_assert_equal_i(0, current_stack);
  cl_assert_equal_p(b, stack[0]);


  // test limited stack size (go to sibling instead of child)
  stack[0] = a;
  current_stack = 0;
  actual = __layer_tree_traverse_next__test_accessor(stack, 1, &current_stack, true);
  cl_assert_equal_p(b, actual);
  cl_assert_equal_i(0, current_stack);
  cl_assert_equal_p(b, stack[0]);

  // test limited stack size (go to sibling of parent instead of child)
  stack[0] = a;
  stack[1] = aa;
  stack[2] = aaa;
  current_stack = 2;
  actual = __layer_tree_traverse_next__test_accessor(stack, 3, &current_stack, true);
  cl_assert_equal_p(ab, actual);
  cl_assert_equal_i(1, current_stack);
  cl_assert_equal_p(a, stack[0]);
  cl_assert_equal_p(ab, stack[1]);

  // not necessary to free memory on unit tests
}

void test_layer__is_ancestor(void) {
  Layer parent, child_a, child_b, child_c;
  Layer *layers[] = {&parent, &child_a, &child_b, &child_c};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }

  layer_add_child(&parent, &child_a);
  cl_assert(layer_is_descendant(&child_a, &parent));
  cl_assert(!layer_is_descendant(&parent, &child_a));
  cl_assert(!layer_is_descendant(&child_b, &parent));

  layer_add_child(&parent, &child_b);
  cl_assert(layer_is_descendant(&child_b, &parent));
  cl_assert(!layer_is_descendant(&child_b, &child_a));
  cl_assert(!layer_is_descendant(&child_c, &child_a));

  layer_add_child(&child_a, &child_c);
  cl_assert(layer_is_descendant(&child_c, &child_a));
  cl_assert(layer_is_descendant(&child_c, &parent));
  cl_assert(!layer_is_descendant(&child_c, &child_b));
}

void test_layer__find_layer_contains_point(void) {
  Layer parent, child_a, child_b, child_c, child_d, child_e, child_f;
  Layer *layers[] = {&parent, &child_a, &child_b, &child_c, &child_d, &child_e, &child_f};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  layer_set_frame(&parent, &GRect(0, 0, 20, 20));
  layer_set_frame(&child_a, &GRect(0, 0, 10, 10));
  layer_set_frame(&child_b, &GRect(2, 2, 6, 6));
  layer_set_frame(&child_c, &GRect(10, 10, 10, 10));
  layer_set_frame(&child_d, &GRect(2, 2, 6, 6));
  layer_set_frame(&child_e, &GRect(10, 10, 10, 10));
  layer_set_frame(&child_f, &GRect(-10, -10, 40, 40));
  layer_add_child(&parent, &child_a);

  cl_assert_equal_p(layer_find_layer_containing_point(&child_a, &GPoint(11, 11)), NULL);
  cl_assert_equal_p(layer_find_layer_containing_point(&child_a, &GPoint(10, 10)), NULL);
  cl_assert_equal_p(layer_find_layer_containing_point(&child_a, &GPoint(9, 9)), &child_a);
  cl_assert_equal_p(layer_find_layer_containing_point(&child_a, &GPoint(0, 0)), &child_a);

  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(9, 9)), &child_a);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(10, 10)), &parent);

  layer_add_child(&child_a, &child_f);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(9, 9)), &child_f);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(1, 1)), &child_f);

  // child layers are subject to their parents' bounds as well as their own (parent layers clip the
  // bounds of child layers)
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(15, 15)), &parent);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(-5, -5)), NULL);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(21, 21)), NULL);
  layer_remove_from_parent(&child_f);

  layer_add_child(&parent, &child_b);
  layer_add_child(&parent, &child_c);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(9, 9)), &child_a);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(6, 6)), &child_b);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(15, 15)), &child_c);

  layer_add_child(&child_a, &child_d);
  layer_add_child(&child_c, &child_e);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(9, 9)), &child_a);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(6, 6)), &child_d);
  // This assertion previously expected &child_e, which asserted that a touch resolves to a layer
  // the renderer never draws under that point. child_e's frame origin (10,10) is relative to
  // child_c, whose own frame origin is (10,10), so layer_render_tree() draws child_e at global
  // (20,20) -- entirely outside the 20x20 clipping parent. child_e is not visible anywhere on
  // this tree, least of all at (15,15). The old expectation was only satisfiable because the
  // traversal failed to rebase by child_c's frame origin on descent; it encoded the defect, not a
  // contract. With the descent corrected, (15,15) lands inside child_c and finds no visible child
  // there, so child_c is the answer. See the coordinate contract in layer.c.
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(15, 15)), &child_c);
}

// A nested layer must be found at the point where it is actually DRAWN, i.e. after composing every
// ancestor's frame origin. Discriminating: without the frame.origin term in the descent this
// resolves to &mid instead of &leaf.
void test_layer__find_layer_contains_point_nonzero_ancestor_origin(void) {
  Layer parent, mid, leaf;
  Layer *layers[] = {&parent, &mid, &leaf};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  layer_set_frame(&parent, &GRect(0, 0, 40, 40));
  layer_set_frame(&mid, &GRect(10, 10, 20, 20));
  layer_set_frame(&leaf, &GRect(5, 5, 10, 10));
  layer_add_child(&parent, &mid);
  layer_add_child(&mid, &leaf);

  // leaf is drawn at global (15,15)-(25,25): mid's origin (10,10) plus leaf's own (5,5).
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(18, 18)), &leaf);
  // Inside mid but before leaf starts.
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(12, 12)), &mid);
  // Inside mid but past leaf's far edge.
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(28, 28)), &mid);
}

// A window-stack transition slides a window by writing a nonzero frame origin on the ROOT layer
// (window_stack_animation_rect.c prv_window_frame_setter, window_stack_animation_round.c). Touch
// events are not gated on window_stack_is_animating, so hit-testing runs while the root origin is
// mid-interpolation. The root is rebased by the same rule as any other node -- there is no special
// case for it.
void test_layer__find_layer_contains_point_nonzero_root_origin(void) {
  Layer root, child;
  Layer *layers[] = {&root, &child};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  // Window slid 20px to the right, part-way through a transition.
  layer_set_frame(&root, &GRect(20, 0, 144, 168));
  layer_set_frame(&child, &GRect(0, 0, 40, 40));
  layer_add_child(&root, &child);

  // The child occupies window-local x [0,40), which is screen x [20,60) once the root is slid.
  // Discriminating: screen x=50 is window-local x=30, inside the child. Without the root rebase
  // the traversal tests the raw screen x=50 against the child's [0,40) frame, misses, and answers
  // &root instead.
  cl_assert_equal_p(layer_find_layer_containing_point(&root, &GPoint(50, 5)), &child);
  // 5px inside the slid window, so 5px into the child.
  cl_assert_equal_p(layer_find_layer_containing_point(&root, &GPoint(25, 5)), &child);
  // Left of the slid window entirely.
  cl_assert_equal_p(layer_find_layer_containing_point(&root, &GPoint(15, 5)), NULL);
  // Window-local x=45, past the 40px-wide child.
  cl_assert_equal_p(layer_find_layer_containing_point(&root, &GPoint(65, 5)), &root);
}

static bool prv_override_layer_contains_point(const Layer *layer, const GPoint *point) {
  return true;
}

void test_layer__find_layer_contains_point_override_layer_contains_point(void) {
  Layer parent, child_a, child_b;
  Layer *layers[] = {&parent, &child_a, &child_b};
  for (int i = 0; i < ARRAY_LENGTH(layers); ++i) {
    layer_init(layers[i], &GRectZero);
  }
  layer_set_frame(&parent, &GRect(0, 0, 20, 20));
  layer_set_frame(&child_a, &GRect(0, 0, 10, 10));
  layer_set_frame(&child_b, &GRect(2, 2, 6, 6));
  layer_add_child(&parent, &child_a);
  layer_add_child(&child_a, &child_b);
  layer_set_contains_point_override(&child_b, prv_override_layer_contains_point);

  cl_assert_equal_p(layer_find_layer_containing_point(&child_b, &GPoint(9, 9)), &child_b);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(9, 9)), &child_b);
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(0, 0)), &child_b);

  // outside the bounds of child a, so child b is not found
  cl_assert_equal_p(layer_find_layer_containing_point(&parent, &GPoint(15, 15)), &parent);
}

void test_layer__recognizer_attach_count(void) {
  s_stub_app_state_recognizer_attach_count = 0;
  s_num_attached_recognizers = 0;
  int dummy;
  Recognizer *r = (Recognizer *)&dummy;
  Layer layer;
  layer_init(&layer, &GRectZero);

  // Attach increments the per-app attach counter
  layer_attach_recognizer(&layer, r);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 1);

  // Detach decrements it back to zero
  layer_detach_recognizer(&layer, r);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);

  // layer_deinit routes destruction through layer_detach_recognizer: exactly ONE
  // decrement (a second decrement in deinit would wrap the uint16_t stub to 65535)
  layer_attach_recognizer(&layer, r);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 1);
  layer_deinit(&layer);
  cl_assert_equal_i(app_state_recognizer_attach_count(), 0);
  cl_assert_equal_i(s_num_attached_recognizers, 0);
}
