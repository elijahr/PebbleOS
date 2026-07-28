/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

/* The clar harness force-includes display_obelix.h into every default-
 * platform test TU; cst816_transform.h includes display_bangle2.h (the
 * single home of the bangle2 letterbox/panel geometry). Undefine the
 * macros the two display headers define DIFFERENTLY so the bangle2 header
 * can define them without redefinition warnings. */
#undef DISPLAY_ORIENTATION_ROTATED_180
#undef DISPLAY_ORIENTATION_ROW_MAJOR_INVERTED
#undef PBL_BW
#undef PBL_COLOR
#undef PBL_DISPLAY_WIDTH
#undef PBL_DISPLAY_HEIGHT
#undef LEGACY_2X_DISP_COLS
#undef LEGACY_2X_DISP_ROWS
#undef LEGACY_3X_DISP_COLS
#undef LEGACY_3X_DISP_ROWS
#undef DISPLAY_FRAMEBUFFER_BYTES

#include "drivers/touch/cst816/cst816_transform.h"

/* Every expected value below is an EXACT integer derived independently from
 * the design formulas (phys_x = 230*cy - 87*cx, phys_y = 88*cx + 105*cy,
 * chip ranges x in [1,238], y in [27,256], panel 176x176, letterbox (16,4),
 * fb 144x168, round-half-up), never from the implementation. Tolerance
 * assertions are deliberately absent: a systematic offset must fail. */

void test_cst816_transform__corners_map_to_expected_fb(void) {
  /* (cx=1,  cy=27):  phys (6123, 2923)  -> panel (50, 0)  -> fb (34, -4->0) */
  cl_assert_equal_i(cst816_transform_fb_x(1, 27), 34);
  cl_assert_equal_i(cst816_transform_fb_y(1, 27), 0);
  /* (cx=238, cy=27): phys (-14496, 23779) -> panel (0, 82) -> fb (-16->0, 78) */
  cl_assert_equal_i(cst816_transform_fb_x(238, 27), 0);
  cl_assert_equal_i(cst816_transform_fb_y(238, 27), 78);
  /* (cx=1, cy=256): phys (58793, 26968) -> panel (176, 94) -> fb (160->143, 90) */
  cl_assert_equal_i(cst816_transform_fb_x(1, 256), 143);
  cl_assert_equal_i(cst816_transform_fb_y(1, 256), 90);
  /* (cx=238, cy=256): phys (38174, 47824) -> panel (126, 176) -> fb (110, 172->167) */
  cl_assert_equal_i(cst816_transform_fb_x(238, 256), 110);
  cl_assert_equal_i(cst816_transform_fb_y(238, 256), 167);
}

void test_cst816_transform__center_maps_near_fb_center(void) {
  /* Chip-range center (120, 141): phys (21990, 25365) -> panel (88, 88).
   * fb center is (71.5, 83.5); expected exact (72, 84) is within 1 of it,
   * and both values pin the letterbox reversal: fb = panel - (16, 4). */
  cl_assert_equal_i(cst816_transform_fb_x(120, 141), 88 - LETTERBOX_OFFSET_X);
  cl_assert_equal_i(cst816_transform_fb_y(120, 141), 88 - LETTERBOX_OFFSET_Y);
  cl_assert_equal_i(cst816_transform_fb_x(120, 141), 72);
  cl_assert_equal_i(cst816_transform_fb_y(120, 141), 84);
}

void test_cst816_transform__pure_vertical_delta_has_zero_x_bleed(void) {
  /* The pure physical-vertical chip direction is (+230, +87):
   * delta_phys_x = 230*87 - 87*230 == 0 exactly, so fb_x must be IDENTICAL
   * (not merely close) across the step at every base point. */
  static const struct {
    int32_t cx;
    int32_t cy;
  } bases[] = {{1, 27}, {5, 100}, {8, 169}};
  for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
    const int32_t cx = bases[i].cx, cy = bases[i].cy;
    cl_assert_equal_i(cst816_transform_fb_x(cx + 230, cy + 87), cst816_transform_fb_x(cx, cy));
  }
  /* And the stepped fb_y values are the exact derived integers. */
  cl_assert_equal_i(cst816_transform_fb_y(1 + 230, 27 + 87), 111);
  cl_assert_equal_i(cst816_transform_fb_y(5 + 230, 100 + 87), 143);
  cl_assert_equal_i(cst816_transform_fb_y(8 + 230, 169 + 87), 167);
}

void test_cst816_transform__out_of_range_inputs_clamp(void) {
  /* (400, 0): phys_x = -34800 -> panel -48 -> fb -64 -> clamp 0 */
  cl_assert_equal_i(cst816_transform_fb_x(400, 0), 0);
  /* (0, 400): phys_x = 92000 -> panel 256 -> fb 240 -> clamp 143 */
  cl_assert_equal_i(cst816_transform_fb_x(0, 400), 143);
  /* (0, 0): phys_y = 0 -> rel -2923 -> panel -10 -> fb -14 -> clamp 0 */
  cl_assert_equal_i(cst816_transform_fb_y(0, 0), 0);
  /* (400, 400): phys_y = 77200 -> panel 291 -> fb 287 -> clamp 167 */
  cl_assert_equal_i(cst816_transform_fb_y(400, 400), 167);
  /* Unclamped components of the same inputs stay exact, not saturated. */
  cl_assert_equal_i(cst816_transform_fb_y(400, 0), 123);
  cl_assert_equal_i(cst816_transform_fb_y(0, 400), 149);
  cl_assert_equal_i(cst816_transform_fb_x(0, 0), 19);
  cl_assert_equal_i(cst816_transform_fb_x(400, 400), 143);
}

void test_cst816_transform__deshear_known_vectors_reproduce_signs(void) {
  /* The four known-vector self-checks documented at the classifier
   * (cst816.c): exact px/py rows plus the >=3:1 dominance and det<0 sign
   * conclusions. px = 87*dx - 230*dy, py = -88*dx - 105*dy. */

  /* chip(-230,-88) -> px=+230, py=+29480 -> vertical (230 < 3*29480), UP */
  cl_assert_equal_i(cst816_deshear_px(-230, -88), 230);
  cl_assert_equal_i(cst816_deshear_py(-230, -88), 29480);
  cl_assert(!(230 >= 3 * 29480));
  cl_assert(cst816_deshear_py(-230, -88) > 0); /* py>0 -> phys -y -> UP */

  /* chip(+231,+85) -> px=+547, py=-29253 -> vertical, DOWN */
  cl_assert_equal_i(cst816_deshear_px(231, 85), 547);
  cl_assert_equal_i(cst816_deshear_py(231, 85), -29253);
  cl_assert(!(547 >= 3 * 29253));
  cl_assert(cst816_deshear_py(231, 85) < 0); /* py<0 -> phys +y -> DOWN */

  /* chip(-112,+75) -> px=-26994, py=+1981 -> horizontal (26994 >= 3*1981), RIGHT */
  cl_assert_equal_i(cst816_deshear_px(-112, 75), -26994);
  cl_assert_equal_i(cst816_deshear_py(-112, 75), 1981);
  cl_assert(26994 >= 3 * 1981);
  cl_assert(cst816_deshear_px(-112, 75) < 0); /* px<0 -> phys +x -> RIGHT */

  /* chip(+99,-101) -> px=+31843, py=+1893 -> horizontal, LEFT */
  cl_assert_equal_i(cst816_deshear_px(99, -101), 31843);
  cl_assert_equal_i(cst816_deshear_py(99, -101), 1893);
  cl_assert(31843 >= 3 * 1893);
  cl_assert(cst816_deshear_px(99, -101) > 0); /* px>0 -> phys -x -> LEFT */

  /* Conventional-sign physical rows are the exact negation of the de-shear
   * rows (det = -29375 < 0), so the two consumers share one mapping. */
  cl_assert_equal_i(cst816_phys_x(99, -101), -cst816_deshear_px(99, -101));
  cl_assert_equal_i(cst816_phys_y(99, -101), -cst816_deshear_py(99, -101));
}
