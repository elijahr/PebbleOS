/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "applib/graphics/graphics.h"
#include "applib/graphics/framebuffer.h"

#include "applib/ui/window_private.h"
#include "applib/ui/layer.h"


#include "clar.h"
#include "util.h"

#include <stdio.h>

// Helper Functions
////////////////////////////////////
#include "test_graphics.h"
#include "${BIT_DEPTH_NAME}/test_framebuffer.h"

// Stubs
////////////////////////////////////
#include "graphics_common_stubs.h"
#include "stubs_applib_resource.h"

static FrameBuffer *fb = NULL;

// Setup
void test_graphics_stroke_circle_${BIT_DEPTH_NAME}__initialize(void) {
  fb = malloc(sizeof(FrameBuffer));
  framebuffer_init(fb, &(GSize) {DISP_COLS, DISP_ROWS});
}

// Teardown
void test_graphics_stroke_circle_${BIT_DEPTH_NAME}__cleanup(void) {
  free(fb);
}

// Tests
////////////////////////////////////

#define RADIUS_BIG 15
#define RADIUS_MEDIUM 8
#define RADIUS_MIN_CALCULATED 3
#define RADIUS_MAX_PRECOMPUTED 2
#define RADIUS_SMALL 1
#define RADIUS_NONE 0
#define STROKE_BIG 10
#define STROKE_SMALL 5
#define STROKE_THREE 3  // In case of fluctuation

#define ORIGIN_RECT_NO_CLIP        GRect(0, 0, 40, 50)
#define ORIGIN_RECT_CLIP_XY        GRect(0, 0, 30, 40)
#define ORIGIN_RECT_CLIP_NXNY      GRect(0, 0, 30, 40)
#define CENTER_OF_ORIGIN_RECT      GPoint(20, 25)
#define CENTER_OF_ORIGIN_RECT_NXNY GPoint(10, 15)

void test_graphics_stroke_circle_${BIT_DEPTH_NAME}__origin_layer(void) {

#if CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  // Big circles
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_BIG);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r16_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_XY, ORIGIN_RECT_CLIP_XY, true, STROKE_BIG);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r16_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_NXNY, ORIGIN_RECT_CLIP_NXNY, true, STROKE_BIG);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r16_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Medium circles
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r8_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_XY, ORIGIN_RECT_CLIP_XY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r8_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_NXNY, ORIGIN_RECT_CLIP_NXNY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r8_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Small circles
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r1_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_XY, ORIGIN_RECT_CLIP_XY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r1_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_CLIP_NXNY, ORIGIN_RECT_CLIP_NXNY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r1_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Testing of the special cases for radius:

  // Radius of 3 - starting point for calculated edges
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_THREE);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_MIN_CALCULATED);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r3_no_clip.${BIT_DEPTH_NAME}.pbi"));

  // Radius of 2 - ending point for precomputed edges
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_THREE);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_MAX_PRECOMPUTED);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r2_no_clip.${BIT_DEPTH_NAME}.pbi"));

  // No circle
  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_THREE);
  graphics_draw_circle(&ctx, CENTER_OF_ORIGIN_RECT_NXNY, RADIUS_NONE);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_origin_aa_r0_no_clip.${BIT_DEPTH_NAME}.pbi"));

#endif // CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

}

#define OFFSET_RECT_NO_CLIP        GRect(10, 10, 40, 50)
#define OFFSET_RECT_CLIP_XY        GRect(10, 10, 30, 40)
#define OFFSET_RECT_CLIP_NXNY      GRect(0, 0, 30, 40)
#define CENTER_OF_OFFSET_RECT      GPoint(10, 15)
#define CENTER_OF_OFFSET_RECT_NXNY GPoint(0, 5)

void test_graphics_stroke_circle_${BIT_DEPTH_NAME}__offset_layer_aa(void) {

#if CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  // Big circles
  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_NO_CLIP, OFFSET_RECT_NO_CLIP, true, STROKE_BIG);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r16_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_XY, OFFSET_RECT_CLIP_XY, true, STROKE_BIG);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r16_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_NXNY, OFFSET_RECT_CLIP_NXNY, true, STROKE_BIG);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT_NXNY, RADIUS_BIG);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r16_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Medium circles
  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_NO_CLIP, OFFSET_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_XY, OFFSET_RECT_CLIP_XY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_NXNY, OFFSET_RECT_CLIP_NXNY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT_NXNY, RADIUS_MEDIUM);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

  // Small circles
  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_NO_CLIP, OFFSET_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r1_no_clip.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_XY, OFFSET_RECT_CLIP_XY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r1_clip_xy.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, OFFSET_RECT_CLIP_NXNY, OFFSET_RECT_CLIP_NXNY, true, STROKE_SMALL);
  graphics_draw_circle(&ctx, CENTER_OF_OFFSET_RECT_NXNY, RADIUS_SMALL);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r1_clip_nxny.${BIT_DEPTH_NAME}.pbi"));

#endif // CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

}

extern void graphics_circle_quadrant_draw_stroked_non_aa(
    GContext* ctx, GPoint p, uint16_t radius, uint8_t stroke_width,
    GCornerMask quadrant);

void test_graphics_stroke_circle_${BIT_DEPTH_NAME}__quadrants(void) {

#if CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerTopLeft);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quad_top_left.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerTopRight);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quad_top_right.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerBottomLeft);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quad_bottom_left.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerBottomRight);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quad_bottom_right.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersTop);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quads_top.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersBottom);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quads_bottom.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersRight);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quads_right.${BIT_DEPTH_NAME}.pbi"));

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, false, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_non_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersLeft);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_r8_quads_left.${BIT_DEPTH_NAME}.pbi"));

#endif // CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

}

extern void graphics_circle_quadrant_draw_stroked_aa(
    GContext* ctx, GPoint p, uint16_t radius, uint8_t stroke_width,
    GCornerMask quadrant);

#if CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

// Compositional-coherence support for __quadrants_aa.
//
// The golden images only assert "output == snapshot of output". The union property below is a
// property of the API contract instead: a half-circle render must be the exact pixel union of its
// two quadrant renders. It holds regardless of what the goldens contain, so it still fails if all
// goldens are regenerated.
//
// KNOWN GAP, DELIBERATE, NOT AN OVERSIGHT: only the .8bit goldens for the aa quadrants exist.
// The non-aa quadrant goldens have .Xbit siblings and these do not. Only the 8bit platform is
// instantiated today (see wscript_build), so the .Xbit set could not be generated from a build
// that never runs. THE DAY A SECOND BIT DEPTH IS RE-ENABLED, THIS TEST FAILS HERE for missing
// goldens -- that is this gap surfacing, not a regression in the renderer. Generate them from
// the new build and inspect before committing; see the commit that added the .8bit set for how
// they were checked (grey-level count, and pixel-union against the non-aa siblings).

#define UNION_BG_ARGB8 (GColorWhiteARGB8)

typedef struct {
  uint8_t *data;
  size_t size;
} FramebufferSnapshot;

static FramebufferSnapshot prv_snapshot_framebuffer(GContext *ctx) {
  const GBitmap *bmp = &ctx->dest_bitmap;
  const size_t size = (size_t)bmp->row_size_bytes * (size_t)bmp->bounds.size.h;
  FramebufferSnapshot snapshot = { .data = malloc(size), .size = size };
  cl_assert(snapshot.data != NULL);
  memcpy(snapshot.data, bmp->addr, size);
  return snapshot;
}

// Asserts half == union(a, b) pixel by pixel. Where exactly one quadrant painted a pixel, the half
// must carry that exact value; where neither did, the half must be background. On the shared seam
// both quadrants paint, and the half legitimately composites their two partial coverages into a
// different (darker) value, so there only membership is asserted.
static void prv_assert_pixel_union(const char *label, const FramebufferSnapshot *half,
                                   const FramebufferSnapshot *a, const FramebufferSnapshot *b) {
  cl_assert_equal_i((int)half->size, (int)a->size);
  cl_assert_equal_i((int)half->size, (int)b->size);

  for (size_t i = 0; i < half->size; ++i) {
    const uint8_t a_val = a->data[i];
    const uint8_t b_val = b->data[i];
    const uint8_t half_val = half->data[i];
    const bool a_painted = (a_val != UNION_BG_ARGB8);
    const bool b_painted = (b_val != UNION_BG_ARGB8);

    uint8_t expected;
    if (a_painted && b_painted) {
      if (half_val != UNION_BG_ARGB8) {
        continue;
      }
      printf("%s: union mismatch at byte offset %zu: half is background (0x%02x) but both "
             "quadrants painted (0x%02x, 0x%02x)\n", label, i, half_val, a_val, b_val);
      cl_fail("half-circle render is missing a pixel painted by both quadrant renders");
      continue;
    } else if (a_painted) {
      expected = a_val;
    } else if (b_painted) {
      expected = b_val;
    } else {
      expected = UNION_BG_ARGB8;
    }

    if (half_val != expected) {
      printf("%s: union mismatch at byte offset %zu: half=0x%02x quad_a=0x%02x quad_b=0x%02x "
             "(expected 0x%02x)\n", label, i, half_val, a_val, b_val, expected);
      cl_fail("half-circle render is not the pixel union of its two quadrant renders");
    }
  }
}

#endif // CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

void test_graphics_stroke_circle_${BIT_DEPTH_NAME}__quadrants_aa(void) {

#if CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

  GContext ctx;
  test_graphics_context_init(&ctx, fb);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerTopLeft);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quad_top_left.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quad_top_left = prv_snapshot_framebuffer(&ctx);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerTopRight);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quad_top_right.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quad_top_right = prv_snapshot_framebuffer(&ctx);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerBottomLeft);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quad_bottom_left.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quad_bottom_left = prv_snapshot_framebuffer(&ctx);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornerBottomRight);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quad_bottom_right.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quad_bottom_right = prv_snapshot_framebuffer(&ctx);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersTop);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quads_top.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quads_top = prv_snapshot_framebuffer(&ctx);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersBottom);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quads_bottom.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quads_bottom = prv_snapshot_framebuffer(&ctx);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersRight);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quads_right.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quads_right = prv_snapshot_framebuffer(&ctx);

  setup_test_aa_sw(&ctx, fb, ORIGIN_RECT_NO_CLIP, ORIGIN_RECT_NO_CLIP, true, STROKE_SMALL);
  graphics_circle_quadrant_draw_stroked_aa(&ctx, CENTER_OF_ORIGIN_RECT, RADIUS_MEDIUM, STROKE_SMALL,GCornersLeft);
  cl_check(gbitmap_pbi_eq(&ctx.dest_bitmap, "stroke_circle_offset_aa_r8_quads_left.${BIT_DEPTH_NAME}.pbi"));
  const FramebufferSnapshot quads_left = prv_snapshot_framebuffer(&ctx);

  // Compositional coherence: each half must be the exact pixel union of its two quadrants.
  prv_assert_pixel_union("quads_top", &quads_top, &quad_top_left, &quad_top_right);
  prv_assert_pixel_union("quads_bottom", &quads_bottom, &quad_bottom_left, &quad_bottom_right);
  prv_assert_pixel_union("quads_left", &quads_left, &quad_top_left, &quad_bottom_left);
  prv_assert_pixel_union("quads_right", &quads_right, &quad_top_right, &quad_bottom_right);

  free(quad_top_left.data);
  free(quad_top_right.data);
  free(quad_bottom_left.data);
  free(quad_bottom_right.data);
  free(quads_top.data);
  free(quads_bottom.data);
  free(quads_left.data);
  free(quads_right.data);

#endif // CONFIG_SCREEN_COLOR_DEPTH_BITS == 8

}
