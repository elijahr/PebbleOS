/* SPDX-FileCopyrightText: 2025 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <inttypes.h>

#include "board/board.h"
#include "drivers/exti.h"
#include "drivers/gpio.h"
#include "drivers/i2c.h"
#include "drivers/rtc.h"
#include "drivers/touch/touch_sensor.h"
#include "kernel/events.h"
#include "kernel/util/sleep.h"
#include "pbl/os/tick.h"
#include "pbl/services/analytics/analytics.h"
#include "pbl/services/new_timer/new_timer.h"
#include "pbl/services/regular_timer.h"
#include "pbl/services/touch/touch.h"
#include "pbl/services/system_task.h"
#include "system/logging.h"
#include "system/passert.h"
#include "pbl/util/math.h"

#include "cst816_fw.h"

PBL_LOG_MODULE_DEFINE(driver_touch_cst816, CONFIG_DRIVER_TOUCH_LOG_LEVEL);

#define CST816_RESET_CYCLE_TIME       10  /* ms */
#define CST816_POR_DELAY_TIME         110 /* ms */
#define CST816_REG_WR_DELAY_TIME      2   /* ms */ 
#define CST816_FW_CHECKSUM_CAL_TIME   500 /* ms */ 

#define CST816_POWER_MODE_REG         0xE5
#define CST816_POWER_MODE_SLEEP       0x03
#define CST816_CHIP_ID_REG            0xA7
#define CST816_CHIP_ID_CST816S        0xB4
#define CST816_CHIP_ID_CST816T        0xB6
#define CST816_FW_VERSION_REG         0xA9
#define CST816_TOUCH_DATA_REG         0x02
#define CST816_TOUCH_DATA_SIZE        5
/* XposH (reg 0x03) bits [7:6] carry the per-frame event type (Zephyr
 * input_cst8xx.c, lupyuen nuttx cst816s driver); bits [3:0] are the coordinate
 * high nibble (12-bit coords). */
#define CST816_EVENT_SHIFT            6
#define CST816_EVENT_DOWN             0
#define CST816_EVENT_LIFTUP           1
#define CST816_EVENT_CONTACT          2
#define CST816_GESTURE_ID             0x01
#define CST816_GESTURE_NONE           0x00
// Standard Hynitron gesture codes. NOTE: on bangle2 the panel runs the lierda
// CST816D blob, which emits these codes rotated 180 degrees (see the
// trajectory recognizer below); the names below reflect the STANDARD meaning.
#define CST816_GESTURE_RIGHT          0x01
#define CST816_GESTURE_LEFT           0x02
#define CST816_GESTURE_DOWN           0x03
#define CST816_GESTURE_UP             0x04
#define CST816_GESTURE_CLICK          0x05
#define CST816_GESTURE_DOUBLE_CLICK   0x0B
#define CST816_GESTURE_LONG_PRESS     0x0C

/* IrqCtl (0xFA): EN_TOUCH (0x40) = IRQ on every touch frame, EN_CHANGE (0x20)
 * = IRQ on touch-state change, EN_MOTION (0x10, gesture-classifier IRQ) left
 * CLEAR so coordinate reporting is continuous and independent of the (broken
 * on the lierda blob) gesture engine. Basis: Zephyr input_cst8xx.c and the
 * CST816S register reference. */
#define CST816_IRQ_CTL_REG            0xFA
#define CST816_IRQ_CTL_VAL            0x60

#define CST816_BOOT_MODE_REG          0xA001
#define CST816_BOOT_MODE_CMD          0xAB
#define CST816_BOOT_FLAG_REG          0xA003
#define CST816_BOOT_FLAG_VAL          0xC1
#define CST816_FW_START_ADDR_REG      0xA014
#define CST816_FW_PAGE_REG            0xA018
#define CST816_FW_PAGE_SIZE           512
#define CST816_FW_PAGE_DONE           0xA004
#define CST816_FW_PAGE_STATE          0xA005
#define CST816_BOOT_EXIT_REG          0xA006
#define CST816_BOOT_EXIT_VAL          0xEE
#define CST816_FW_PAGE_READY          0x55
#define CST816_FW_WR_TIME             100 /* ms */
#define CST816_FW_CHECKSUM_REG        0xA008
#define CST816_FW_VER_INFO_INDEX      (-11)

/* Workaround: the CST816 occasionally wedges and stops asserting its INT line.
 * If no touch activity is seen between two watchdog checks, hard-reset it. */
#define CST816_WATCHDOG_PERIOD_MIN    30

/* The chip stays awake for 2s after a wake; an interrupt seen >=2s after the
 * previous one therefore marks a fresh sleep->awake transition. */
#define CST816_WAKE_SPACING_MS        2000

#ifdef CONFIG_BOARD_BANGLE2
/* Shared de-shear core + chip->framebuffer mapping. Bangle2-specific: the
 * affine was measured on a bangle2 unit and consumes bangle2's letterbox
 * geometry, so it must never be inherited by another nRF52 board. */
#include "drivers/touch/cst816/cst816_transform.h"
#endif

#ifdef CONFIG_SOC_NRF52
/* TEMPORARY bangle2 touch debug: ring buffer of raw CST816 events, read
 * post-hoc over SWD. 12 bytes per entry, 256 entries (0xC00 bytes total,
 * large enough to capture a full fragmentation burst). Remove before merge. */
typedef struct {
  uint32_t tick;        /* low 32 bits of rtc_get_ticks() at processing time */
  uint8_t raw_gesture;  /* raw gesture ID register (0x01) */
  uint8_t fingers;      /* raw finger-count register (0x02), count in low nibble */
  uint16_t x;           /* decoded X (after axis inversion) */
  uint16_t y;           /* decoded Y (after axis inversion) */
  uint8_t action;       /* dispatched TouchGesture_*; 0xFE = suppressed
                         * (lockout/debounce/reversal); 0xFF = none. Dispatch
                         * happens in a synthetic finalize entry (fingers ==
                         * 0xF0), not in per-frame entries. */
  uint8_t evt;          /* bits[1:0] = XposH event type (0 Down / 1 LiftUp /
                         * 2 Contact); 0x10 = coords rejected (LiftUp garbage
                         * or edge blip); 0x20 = coalesced continuation;
                         * 0xF1 = synthetic finalize entry */
} Cst816TouchLog;

static volatile Cst816TouchLog s_cst816_touchlog[256];
static volatile uint32_t s_cst816_touchlog_count; /* total events; slot = count & 255 */

/* WORKAROUND (bangle2, lierda CST816D blob): trajectory-based gesture
 * recognizer; exactly ONE gesture per finger-stroke, decided on release.
 *
 * The wrong-panel lierda blob is unusable as a gesture source. Frame-level
 * evidence (SWD touchlog captures; chip coord space x in [1,238] clamping
 * to exactly 0/238, y in [27,256]; frame period ~12-15 ms, ~72 Hz):
 *   - DIRECTION CODES UNRELIABLE: three physically identical swipes carried
 *     raw codes 02/04/02, and a |dx|+|dy|=65 flick was classified as a
 *     swipe purely by its raw code. The raw gesture register is therefore
 *     IGNORED for classification; only the tracked trajectory decides.
 *   - PHANTOM CONTACTS: 2-frame / ~12 ms / zero-displacement contacts
 *     pinned at the x==238 clamp appear ~242 ms after an orphan release
 *     during fast swiping (one such phantom tap opened Music). Genuine
 *     strokes measured >=6 frames, >=69 ms, max |dx|+|dy| 161-367.
 *   - FRAGMENTATION: one physical swipe splits into fragments 242-407 ms
 *     apart (orphan releases, orphan 2-frame presses, mid-stroke 1->2
 *     finger ghosts near edges). Intentional consecutive human gestures
 *     measured >=436 ms apart (436/478/521/662).
 *   - EDGE CLAMPING: 7/14 strokes began pinned at x==238 (finger enters
 *     from the bezel; 2-4 identical clamped frames precede real motion),
 *     and strokes can end abruptly clamped at an edge.
 *
 * Recognizer rules (constants below; all cite the measurements above):
 *   1. Per stroke, anchor the down point at the FIRST NON-edge-clamped
 *      frame (skip leading x==0 / x==238 frames), then track the running
 *      max of |dx|+|dy| from that anchor, the (dx,dy) vector at that max,
 *      the frame count, and the last point seen.
 *   2. Direction comes from the max-displacement vector, DE-SHEARED from
 *      the chip's skewed coordinate frame into the PHYSICAL frame by
 *      inverting the measured phys->chip affine (prv_phys_from_deshear).
 *      Data validation (2026-07 touchlog): deliberate full-lift swipes
 *      classified 100% correctly under the old quadrant rule, but
 *      ~45-degree swipes mis-fired BACK because direction was tested in
 *      the chip's skewed frame, where |dx|/|dy| straddles 1.0 and is
 *      unusable. De-shearing plus an asymmetric dominance gate (>=3:1
 *      dominance required for horizontal since BACK is the destructive
 *      misfire; EVERYTHING else, diagonals included, is vertical since a
 *      mis-up/down is benign but a dropped swipe is not) fixes this.
 *      There is NO direction dead zone (2026-07 field retest: one made
 *      swipes take ~4 attempts to register). This is a de-shear, NOT an
 *      axis swap or rotation.
 *   3. SWIPE if the EFFECTIVE displacement >= CST816_SWIPE_MIN_DISP AND
 *      the stroke spans >= CST816_SWIPE_MIN_FRAMES frames, where
 *      effective = max(disp from the trimmed non-clamped anchor, disp from
 *      the stroke's FIRST valid frame INCLUDING leading edge-clamped
 *      frames). Leading x==0/238 frames are real early travel and count
 *      toward swipe qualification (2026-07 touchlog: trimming them dropped
 *      a genuine swipe just under threshold). TAP only if the strict
 *      trimmed-anchor disp <= CST816_TAP_MAX_DISP (so edge noise cannot
 *      fake a large tap), the stroke spans >= CST816_TAP_MIN_FRAMES frames,
 *      lasted CST816_TAP_MIN_MS..CST816_TAP_MAX_MS, captured at least one
 *      non-edge-clamped frame, and is not a 1-2-frame zero-motion contact
 *      pinned at an x clamp (the measured phantom signature). Displacement
 *      in (CST816_TAP_MAX_DISP, CST816_SWIPE_MIN_DISP), or a would-be
 *      swipe under CST816_SWIPE_MIN_FRAMES frames, is a DEAD ZONE:
 *      ambiguous, dispatch NOTHING -- a weak gesture must never SELECT,
 *      and rapid 2-frame corner blips / tiny return flicks must never
 *      swipe. (Very-rapid swipes are dropped by the CHIP itself before
 *      any frames reach us -- unrecoverable in the driver; ignored.)
 *   4. TIME GATES, split by class (2026-07 touchlog): a TAP whose stroke
 *      STARTED within CST816_TAP_LOCKOUT_MS of the previous dispatch is a
 *      fragment of that gesture and is suppressed (logged 0xFE). SWIPES
 *      are NEVER subject to that lockout -- intentional swipe cadence
 *      measured down to 352 ms overlaps the 420 ms window, and the old
 *      shared lockout ate 2 of 6 real swipes -- only a short
 *      CST816_SWIPE_DEBOUNCE_MS safety debounce applies to swipes.
 *   5. A mid-stroke fingers 1->2 transition is a CONTINUATION of the same
 *      stroke (ghost second finger); only a release (rule 7) ends a stroke.
 *   6. Dispatch happens exactly once, when a stroke is FINALIZED -- never
 *      mid-stroke, and structurally never per raw code.
 *   7. EVENT-TYPE BITS (XposH[7:6]: 0=Down, 1=LiftUp, 2=Contact; Zephyr
 *      input_cst8xx.c): a LiftUp frame OR fingers==0 releases the stroke.
 *      LiftUp frames carry GARBAGE coordinates (lupyuen documented; the
 *      source of the phantom edge taps), so their x/y are NEVER fed to the
 *      trajectory -- the last valid Down/Contact position is the release
 *      point.
 *   8. EDGE-BLIP FILTER (Espruino jswrap_bangle.c:2100): frames whose x is
 *      outside the physical panel (>= CST816_COORD_X_LIMIT in the ~240-wide
 *      frame) are corrupt; ignore their coordinates entirely.
 *   9. RELEASE COALESCING: a finger slide makes the chip drop + re-grab
 *      contact (fragmentation above). A release is therefore only
 *      PENDING for CST816_COALESCE_MS; a new contact inside that window
 *      CONTINUES the same stroke (original down-anchor kept). Only when
 *      the window expires with no re-contact is the stroke finalized and
 *      dispatched (deferred via new_timer -> system task).
 *  10. REVERSAL SUPPRESSION (2026-07 touchlog): between consecutive
 *      same-direction swipes the finger flicking back to its start
 *      position registers as a stroke and fired the REVERSE direction.
 *      A finalized VERTICAL swipe whose physical direction OPPOSES the
 *      last ACCEPTED vertical dispatch, and whose stroke STARTED within
 *      CST816_REVERSAL_WINDOW_MS of that dispatch, is suppressed (logged
 *      0xFE). Comparison is against the last ACCEPTED dispatch, never a
 *      suppressed one, so the next genuine same-direction swipe (which
 *      matches the last accepted) still fires, and a deliberate reversal
 *      made after the window still fires. Vertical only: LEFT already
 *      dispatches nothing and BACK (RIGHT) has no opposite in the map.
 *
 * Physical gesture -> emitted TouchGesture -> nav action (touch.c shim):
 *   TAP            -> TouchGesture_Tap        -> SELECT
 *   swipe UP       -> TouchGesture_SwipeDown  -> BUTTON_ID_DOWN (inverted
 *                     scroll: up-swipe moves the selection down)
 *   swipe DOWN     -> TouchGesture_SwipeUp    -> BUTTON_ID_UP
 *   swipe RIGHT    -> TouchGesture_SwipeRight -> BACK
 *   swipe LEFT     -> (nothing, explicitly ignored)
 */

/* X values the blob clamps to; frames pinned here carry no position
 * information (finger under the bezel, or a ghost). Measured: real x spans
 * [1,238]; clamped frames read exactly 0 or 238. */
#define CST816_EDGE_CLAMP_X_LO    0
#define CST816_EDGE_CLAMP_X_HI    238

/* Swipe threshold on effective max |dx|+|dy| (2026-07 live touchlog):
 * genuine swipes measured 71-331 but some register lower/short; genuine
 * taps ~7 (ceiling 25). 45 catches the weak/short real swipes that 60
 * still dropped (field retest: ~4 attempts per registration) while
 * staying well above the tap band. */
#define CST816_SWIPE_MIN_DISP     45

/* Swipe frame floor (rule 3): rapid 2-frame corner blips and tiny return
 * strokes must dispatch NOTHING by rule; genuine strokes measured >=6
 * frames. (Very-rapid swipes are dropped by the chip before any frames
 * arrive -- unrecoverable, ignored.) */
#define CST816_SWIPE_MIN_FRAMES   3

/* Tap displacement ceiling on the STRICT trimmed-anchor max |dx|+|dy|
 * (genuine taps measured ~7). Displacement in (CST816_TAP_MAX_DISP,
 * CST816_SWIPE_MIN_DISP) is a DEAD ZONE: ambiguous, dispatch nothing --
 * a weak gesture must never turn into a SELECT. */
#define CST816_TAP_MAX_DISP       25

/* Tap validity gate: genuine strokes measured >=6 frames / >=69 ms;
 * phantoms 2 frames / ~12 ms. */
#define CST816_TAP_MIN_FRAMES     3
#define CST816_TAP_MIN_MS         40
#define CST816_TAP_MAX_MS         500

/* Phantom signature: at most this many frames, all pinned at an x clamp,
 * zero displacement (measured: 2 frames / 12 ms / max disp 0 / x==238). */
#define CST816_PHANTOM_MAX_FRAMES 2

/* Tap release-point rail margin: a TAP whose last valid coordinate sits on
 * or next to an x clamp rail (x <= LO+margin or x >= HI-margin) is chip
 * tracking-loss garbage, not a finger -- kills phantom edge-rail SELECTs.
 * Taps only; swipes are unaffected. */
#define CST816_TAP_RAIL_MARGIN    2

/* Chip coordinate space upper bound (measured real x in [1,238]; the panel
 * is ~240 px). Anything outside [0, this] is off-panel garbage. */
#define CST816_PANEL_MAX_COORD    239

/* Edge-blip filter (rule 8): the panel is ~240 px wide (measured real x in
 * [1,238]); any frame reporting x at/above this is corrupt, not a touch
 * (Espruino jswrap_bangle.c:2100 rejects the same way). */
#define CST816_COORD_X_LIMIT      250

/* Release-coalescing window (rule 9): how long a release stays PENDING
 * before the stroke is finalized. Long enough to bridge the chip's
 * drop+re-grab during a slide (one missed ~72 Hz frame ~= 14 ms; allow a
 * few), short enough to add no perceptible dispatch latency. Basis:
 * esp_lcd_touch / RIOT treat sub-50ms re-contacts as the same touch. */
#define CST816_COALESCE_MS        50

/* TAP-ONLY repeat lockout: fragments of one physical gesture arrived
 * <=407 ms after the previous dispatch, and a fragment misread as a tap
 * fires a spurious SELECT, so a would-be TAP whose stroke started inside
 * this window is suppressed (logged 0xFE). The measured fragment gaps
 * (242-407 ms) far exceed the 50 ms coalescing window, so this lockout is
 * still the only defense against slow tap fragments. It applies to TAPS
 * ONLY: intentional swipe cadence measured 352-703 ms (2026-07 touchlog)
 * overlaps 420 ms, so time cannot separate fast intentional swipes from
 * fragments -- the old any-gesture lockout ate 2 of 6 real swipes. */
#define CST816_TAP_LOCKOUT_MS     420

/* SWIPE safety debounce: the ONLY time gate on swipe dispatches. Well
 * below the fastest measured intentional swipe cadence (>=352 ms between
 * dispatches) so real fast scrolling is never throttled, while still
 * blocking any pathological sub-100 ms fragmentation-into-swipes burst. */
#define CST816_SWIPE_DEBOUNCE_MS  100

/* Reversal-suppression window (rule 10): return strokes fire within ~1 s
 * of the accepted swipe; deliberate reversals are usually slower. The
 * original 2.0 s window also ate intentional quick direction changes
 * (2026-07 field retest), so this is a lighter 800 ms safety net.
 * Applies only to a vertical swipe opposing the last ACCEPTED vertical
 * dispatch (see rule 10 for why accepted, not last). */
#define CST816_REVERSAL_WINDOW_MS 800

/* Physical (user-perceived) gesture direction. */
typedef enum {
  Cst816Phys_None,
  Cst816Phys_Tap,
  Cst816Phys_Up,
  Cst816Phys_Down,
  Cst816Phys_Left,
  Cst816Phys_Right,
} Cst816Phys;

/* Per-stroke tracking state (rule 1). */
static bool s_stroke_active = false;
static bool s_down_captured = false; /* a non-edge-clamped frame was seen */
static GPoint s_down_point;          /* first NON-edge-clamped point */
static RtcTicks s_down_ticks;        /* ticks at s_down_point */
static RtcTicks s_start_ticks;       /* ticks at the stroke's first frame */
static GPoint s_last_point;          /* last point seen with fingers >= 1 */
static GPoint s_last_unclamped_point; /* last NON-edge-clamped point; feeds the
                                       * coordinate path when a mid-stroke frame
                                       * is pinned at an x clamp; only read while
                                       * s_down_captured is set, which guarantees
                                       * a same-stroke write preceded the read */
static uint16_t s_frame_count;       /* frames seen this stroke */
static int32_t s_max_disp;           /* running max |dx|+|dy| from down point */
static int16_t s_max_dx;             /* dx at max displacement */
static int16_t s_max_dy;             /* dy at max displacement */
/* Edge-travel credit (rule 3): a second anchor at the stroke's FIRST valid
 * frame, INCLUDING leading edge-clamped x==0/238 frames -- those carry real
 * early travel for swipe qualification (a genuine swipe measured just under
 * threshold when they were trimmed). Used ONLY for the swipe >= threshold
 * test; the tap test keeps the strict trimmed anchor above. */
static bool s_first_captured = false; /* a valid (possibly clamped) frame seen */
static GPoint s_first_point;          /* stroke's first valid frame */
static int32_t s_max_disp_first;      /* running max |dx|+|dy| from s_first_point */
static int16_t s_max_dx_first;        /* dx at that max */
static int16_t s_max_dy_first;        /* dy at that max */
static bool s_all_frames_clamped;    /* every frame pinned at an x clamp */
static uint8_t s_stroke_code;        /* last raw swipe code; DIAGNOSTIC ONLY,
                                      * never drives classification */
/* Release-coalescing state (rule 9). While s_pending_release is set the
 * stroke saw a release but may still be continued by a re-contact within
 * CST816_COALESCE_MS; s_release_ticks is when the release arrived (also the
 * stroke's classification end time). */
static bool s_pending_release = false;
static RtcTicks s_release_ticks;
static TimerID s_coalesce_timer = TIMER_INVALID_ID;
/* Bumped every time a release is armed; a deferred finalize only acts if its
 * generation still matches, so a stale timer callback can never finalize a
 * LATER stroke's pending release early. */
static uint32_t s_release_generation;
/* Repeat-lockout anchor (rule 4). Deliberately NOT cleared by
 * prv_stroke_reset(): the lockout must survive stroke teardown. */
static RtcTicks s_last_dispatch_ticks;
/* Reversal-suppression anchor (rule 10): direction and tick of the last
 * ACCEPTED (actually dispatched, never suppressed) VERTICAL swipe.
 * Deliberately NOT cleared by prv_stroke_reset(): must survive stroke
 * teardown, exactly like the lockout anchor above. */
static Cst816Phys s_last_vert_phys = Cst816Phys_None;
static RtcTicks s_last_vert_ticks;

/* Forget any in-progress stroke (finalize, error recovery, chip reset). */
static void prv_stroke_reset(void) {
  s_stroke_active = false;
  s_pending_release = false;
  s_down_captured = false;
  s_last_unclamped_point = GPointZero;
  s_frame_count = 0;
  s_max_disp = 0;
  s_max_dx = 0;
  s_max_dy = 0;
  s_first_captured = false;
  s_max_disp_first = 0;
  s_max_dx_first = 0;
  s_max_dy_first = 0;
  s_all_frames_clamped = true;
  s_stroke_code = 0;
}

/* De-sheared direction classifier (rule 2). (dx,dy) is the chosen
 * max-displacement trajectory vector in CHIP units.
 *
 * The measured phys->chip affine maps unit physical strokes to chip
 * deltas: phys_x -> chip(-105,+88), phys_y -> chip(+230,+87). Inverting
 * that 2x2 with integer math (det = (-105)(87) - (88)(230) = -29375):
 *   px = 87*dx - 230*dy;    // = phys_x * det
 *   py = -88*dx - 105*dy;   // = phys_y * det
 * det < 0, so the true physical signs are the NEGATED signs of px/py;
 * the shared |det| factor cancels in the |px| vs |py| ratio tests.
 * Physical-frame meaning (validated on captured data): phys +x = RIGHT,
 * phys -x = LEFT, phys +y = DOWN, phys -y = UP.
 *
 * Known-vector self-check (hand-computed while coding, all pass):
 *   chip(-230,-88)  -> px=+230,   py=+29480 -> vertical, UP
 *   chip(+231,+85)  -> px=+547,   py=-29253 -> vertical, DOWN
 *   chip(-112,+75)  -> px=-26994, py=+1981  -> horizontal, RIGHT
 *   chip(+99,-101)  -> px=+31843, py=+1893  -> horizontal, LEFT
 *
 * Asymmetric dominance gate, NO dead zone (2026-07 field retest: the
 * former 1:1..3:1 ambiguous-diagonal dead zone dropped genuine swipes,
 * forcing ~4 attempts per registration; a misfired up/down is benign but
 * a dropped swipe is not). BACK (physical RIGHT) is the destructive
 * misfire, so ONLY a strongly-horizontal stroke (>=3:1 dominance) may
 * classify horizontal; EVERYTHING else is vertical. This is a de-shear,
 * NOT an axis swap or rotation. */
static Cst816Phys prv_phys_from_deshear(int32_t dx, int32_t dy) {
  const int32_t px = cst816_deshear_px(dx, dy); /* phys_x * det (det = -29375) */
  const int32_t py = cst816_deshear_py(dx, dy); /* phys_y * det */
  const int32_t apx = ABS(px);
  const int32_t apy = ABS(py);

  if (apx >= 3 * apy) {
    /* Strong horizontal dominance. det < 0: physical sign is -sign(px). */
    return (px < 0) ? Cst816Phys_Right : Cst816Phys_Left;
  }
  /* Everything else is vertical (apy > 0 here: apy == 0 implies the
   * horizontal branch taken). det < 0: physical sign is -sign(py).
   * py > 0 -> phys -y -> UP. Diagonals land here by design: misfiring
   * up/down is benign, dropping the swipe is not. */
  return (py > 0) ? Cst816Phys_Up : Cst816Phys_Down;
}

/* Finalize a pending-release stroke: classify, dispatch at most one gesture,
 * tear the stroke down (rules 3/4/6). Runs ONLY on the system task -- either
 * from the frame path (late re-contact) or via the coalesce timer's
 * system-task hop -- so all stroke state stays single-threaded. */
static void prv_finalize_stroke(void) {
  if (!s_stroke_active || !s_pending_release) {
    return; /* already continued or torn down; the deferred finalize is stale */
  }

  /* Duration runs from the down anchor when one was captured, else from the
   * stroke's first (clamped) frame, and ends at the release frame. */
  const RtcTicks dur = s_release_ticks - (s_down_captured ? s_down_ticks : s_start_ticks);

  /* Measured phantom signature: 1-2 frames, pinned at an x clamp, zero
   * displacement (rule 3). */
  const bool phantom = (s_frame_count <= CST816_PHANTOM_MAX_FRAMES) &&
                       s_all_frames_clamped && (s_max_disp == 0);

  /* Rule 3: swipe qualification credits edge travel -- effective disp is
   * the larger of the trimmed-anchor max and the first-frame max (leading
   * edge-clamped frames included). Direction comes from the quadrant of
   * whichever vector produced the chosen max. The TAP test below keeps the
   * strict trimmed-anchor s_max_disp so edge noise cannot fake a large tap. */
  const bool first_anchor_wins = (s_max_disp_first > s_max_disp);
  const int32_t eff_disp_for_swipe = first_anchor_wins ? s_max_disp_first : s_max_disp;

  /* Kills phantom edge-rail SELECTs: a would-be TAP whose last valid
   * coordinate sits on/next to an x clamp rail or off-panel is chip
   * tracking-loss garbage misclassified as a tap (these open animated
   * windows and trigger the runaway loop). TAP branch only; swipes keep
   * their edge-travel credit untouched. */
  const bool release_on_rail =
      (s_last_point.x <= (CST816_EDGE_CLAMP_X_LO + CST816_TAP_RAIL_MARGIN)) ||
      (s_last_point.x >= (CST816_EDGE_CLAMP_X_HI - CST816_TAP_RAIL_MARGIN)) ||
      (s_last_point.x > CST816_PANEL_MAX_COORD) ||
      (s_last_point.y < 0) || (s_last_point.y > CST816_PANEL_MAX_COORD);

  Cst816Phys phys = Cst816Phys_None;
  if ((eff_disp_for_swipe >= CST816_SWIPE_MIN_DISP) &&
      (s_frame_count >= CST816_SWIPE_MIN_FRAMES)) {
    /* SWIPE: direction from the chosen max-displacement vector,
     * de-sheared into the physical frame (rule 2); always classifies
     * (no direction dead zone). Rapid 2-frame corner blips fail the
     * frame floor and dispatch nothing by rule (rule 3). */
    phys = first_anchor_wins
               ? prv_phys_from_deshear(s_max_dx_first, s_max_dy_first)
               : prv_phys_from_deshear(s_max_dx, s_max_dy);
  } else if ((s_max_disp <= CST816_TAP_MAX_DISP) &&
             !phantom && s_down_captured && !release_on_rail &&
             (s_frame_count >= CST816_TAP_MIN_FRAMES) &&
             (dur >= milliseconds_to_ticks(CST816_TAP_MIN_MS)) &&
             (dur <= milliseconds_to_ticks(CST816_TAP_MAX_MS))) {
    /* TAP: genuine taps measured maxdisp ~7, so require the strict
     * trimmed-anchor disp <= CST816_TAP_MAX_DISP, plus at least one real
     * (non-edge-clamped) sample, and provably a finger, not a 2-frame
     * edge-pinned phantom (rules 3/8). */
    phys = Cst816Phys_Tap;
  }
  /* else: dispatch nothing -- DEAD ZONE (CST816_TAP_MAX_DISP < disp <
   * CST816_SWIPE_MIN_DISP: ambiguous motion must never SELECT; or a
   * would-be swipe under CST816_SWIPE_MIN_FRAMES frames: rapid corner
   * blips and tiny return flicks), phantom, too-short blip, or long
   * dwell. */

  /* Physical -> emitted TouchGesture (see mapping table above). */
  int dispatched = -1;
  switch (phys) {
    case Cst816Phys_Tap:
      dispatched = TouchGesture_Tap;        /* -> SELECT */
      break;
    case Cst816Phys_Up:
      dispatched = TouchGesture_SwipeDown;  /* -> BUTTON_ID_DOWN (inverted) */
      break;
    case Cst816Phys_Down:
      dispatched = TouchGesture_SwipeUp;    /* -> BUTTON_ID_UP (inverted) */
      break;
    case Cst816Phys_Right:
      dispatched = TouchGesture_SwipeRight; /* -> BACK */
      break;
    case Cst816Phys_Left:                   /* explicitly ignored */
    case Cst816Phys_None:
    default:
      break;
  }

  /* Rule 4: time gates, split by gesture class (2026-07 touchlog).
   * TAP: a would-be tap whose stroke STARTED within CST816_TAP_LOCKOUT_MS
   * of the previous dispatch is a fragment of that gesture (fragments
   * measured <=407 ms after a dispatch) -- suppress it (logged 0xFE).
   * SWIPE: NEVER the 420 ms lockout (intentional swipe cadence measured
   * 352-703 ms overlaps it; the old shared lockout ate 2 of 6 real
   * swipes) -- only the CST816_SWIPE_DEBOUNCE_MS safety debounce, far
   * below real cadence, against pathological sub-100 ms fragment bursts. */
  bool lockout_hit = false;
  if ((dispatched >= 0) && (s_last_dispatch_ticks != 0)) {
    const uint32_t gate_ms = (phys == Cst816Phys_Tap) ? CST816_TAP_LOCKOUT_MS
                                                      : CST816_SWIPE_DEBOUNCE_MS;
    if ((s_start_ticks - s_last_dispatch_ticks) < milliseconds_to_ticks(gate_ms)) {
      lockout_hit = true;
      dispatched = -1;
    }
  }

  /* Rule 10: reversal suppression. Between consecutive same-direction
   * swipes the finger's return flick registers as a stroke and fired the
   * REVERSE direction (specimen: reverse at 1.32 s after the accepted
   * dispatch, vs the closest intentional reversal at 4.59 s -- hence the
   * 2.0 s window). Suppress a VERTICAL swipe opposing the last ACCEPTED
   * vertical dispatch when this stroke STARTED inside the window. The
   * anchor is the last ACCEPTED dispatch, never a suppressed one, so the
   * next genuine same-direction swipe still fires and a deliberate
   * reversal after the window still fires. Vertical only: LEFT dispatches
   * nothing and BACK (RIGHT) has no opposite in the emitted map. */
  bool reversal_hit = false;
  if ((dispatched >= 0) &&
      ((phys == Cst816Phys_Up) || (phys == Cst816Phys_Down)) &&
      ((s_last_vert_phys == Cst816Phys_Up) || (s_last_vert_phys == Cst816Phys_Down)) &&
      (phys != s_last_vert_phys) &&
      ((s_start_ticks - s_last_vert_ticks) <
       milliseconds_to_ticks(CST816_REVERSAL_WINDOW_MS))) {
    reversal_hit = true;
    dispatched = -1;
  }

  if (dispatched >= 0) {
    s_last_dispatch_ticks = s_release_ticks;
    if ((phys == Cst816Phys_Up) || (phys == Cst816Phys_Down)) {
      /* Rule 10: update the reversal anchor ONLY on an actually-dispatched
       * (accepted) vertical swipe. */
      s_last_vert_phys = phys;
      s_last_vert_ticks = s_release_ticks;
    }
  }

  PBL_LOG_DBG("stroke: frames=%" PRIu16 " maxdisp=%" PRId32 " firstdisp=%" PRId32
              " vec=(%" PRId16 ",%" PRId16 ") code=0x%02X phys=%d%s%s",
              s_frame_count, s_max_disp, s_max_disp_first, s_max_dx, s_max_dy,
              s_stroke_code, (int)phys, lockout_hit ? " LOCKOUT" : "",
              reversal_hit ? " REVERSAL" : "");

  /* TEMPORARY bangle2 touch debug: synthetic finalize entry (fingers=0xF0,
   * evt=0xF1) carrying the dispatch decision; release point in x/y. */
  {
    volatile Cst816TouchLog *slot = &s_cst816_touchlog[s_cst816_touchlog_count & 255];
    slot->tick = (uint32_t)s_release_ticks;
    slot->raw_gesture = s_stroke_code;
    slot->fingers = 0xF0;
    slot->x = (uint16_t)s_last_point.x;
    slot->y = (uint16_t)s_last_point.y;
    /* 0xFE = suppressed (tap lockout, swipe debounce, or rule-10 reversal). */
    slot->action = (dispatched >= 0) ? (uint8_t)dispatched
                                     : ((lockout_hit || reversal_hit) ? 0xFE : 0xFF);
    slot->evt = 0xF1;
    s_cst816_touchlog_count++;
  }

  /* Release point: the LAST valid Down/Contact position (rule 7) -- never a
   * LiftUp frame's garbage coordinates. */
  GPoint release_point = s_last_point;
#ifdef CONFIG_BOARD_BANGLE2
  /* Chip -> framebuffer space, same mapping as the update path, so both touch
   * service entry points deliver framebuffer coordinates. */
  release_point = GPoint(cst816_transform_fb_x(release_point.x, release_point.y),
                         cst816_transform_fb_y(release_point.x, release_point.y));
#endif

  prv_stroke_reset();

  if (dispatched >= 0) {
    touch_handle_gesture((TouchGesture)dispatched, release_point.x, release_point.y);
  }
}

/* System-task hop for the coalesce timer (all stroke state is owned by the
 * system task). context carries the release generation the timer was armed
 * for; a mismatch means the stroke was already continued/finalized. */
static void prv_finalize_stroke_sys_cb(void *context) {
  if ((uint32_t)(uintptr_t)context != s_release_generation) {
    return;
  }
  prv_finalize_stroke();
}

/* NewTimer thread: coalesce window expired without a re-contact. */
static void prv_coalesce_timer_cb(void *data) {
  system_task_add_callback(prv_finalize_stroke_sys_cb, data);
}

static bool prv_read_data(uint16_t register_address, uint8_t *result, uint16_t size,
                          bool is_work_mode);
static bool prv_write_data(uint16_t register_address, const uint8_t *datum, uint16_t size,
                           bool is_work_mode);

/* Rule/upgrade 4 (research): put the chip in continuous coordinate-reporting
 * mode -- IrqCtl (0xFA) = EN_TOUCH|EN_CHANGE, motion/gesture IRQ bit clear --
 * so frames flow independent of the broken lierda gesture engine. Must be
 * re-applied after EVERY hard reset (POR restores the blob default), so it is
 * called from init, enable, and the idle-watchdog recovery path. Readback
 * verifies the blob honored the write; failure is logged, NEVER fatal. */
static void prv_config_irq_ctl(void) {
  uint8_t val = CST816_IRQ_CTL_VAL;
  if (!prv_write_data(CST816_IRQ_CTL_REG, &val, 1, 1)) {
    PBL_LOG_WRN("CST816 IrqCtl(0xFA) write NAKed; continuous reporting not confirmed");
    return;
  }
  psleep(CST816_REG_WR_DELAY_TIME);
  uint8_t readback = 0;
  if (!prv_read_data(CST816_IRQ_CTL_REG, &readback, 1, 1)) {
    PBL_LOG_WRN("CST816 IrqCtl(0xFA) readback failed");
  } else if (readback != CST816_IRQ_CTL_VAL) {
    PBL_LOG_WRN("CST816 IrqCtl(0xFA) did not stick: wrote 0x%02X, read 0x%02X",
                CST816_IRQ_CTL_VAL, readback);
  } else {
    PBL_LOG_DBG("CST816 IrqCtl(0xFA)=0x%02X (continuous reporting)", readback);
  }
}
#endif

#ifndef CONFIG_SOC_NRF52
/* Map the raw gesture register (0x01) to a TouchGesture, or -1 for none.
 * Standard Hynitron mapping (SiFli boards: getafix/obelix). On bangle2
 * (nRF52) the lierda blob's codes are unreliable and rotated; the
 * trajectory recognizer above is used instead. */
static int prv_decode_gesture(uint8_t id) {
  switch (id) {
    case CST816_GESTURE_CLICK:
      return TouchGesture_Tap;
    case CST816_GESTURE_DOUBLE_CLICK:
      return TouchGesture_DoubleTap;
    case CST816_GESTURE_UP:
      return TouchGesture_SwipeUp;
    case CST816_GESTURE_DOWN:
      return TouchGesture_SwipeDown;
    case CST816_GESTURE_RIGHT:
      return TouchGesture_SwipeRight;
    default:
      return -1;
  }
}
#endif

static bool s_callback_scheduled = false;
static bool s_enabled = false;
static bool s_reset_scheduled = false;
static bool s_activity_since_check = false;
static RtcTicks s_last_irq_ticks = 0;
static PebbleMutex *s_i2c_lock;

static void prv_exti_cb(bool *should_context_switch);
static void cst816_hw_reset(void);
static void prv_watchdog_cb(void *data);

static RegularTimerInfo s_watchdog_timer = {
  .cb = prv_watchdog_cb,
};

static bool prv_read_data(uint16_t register_address, uint8_t *result, uint16_t size, bool is_work_mode) {
  mutex_lock(s_i2c_lock);
  I2CSlavePort* port = CST816->i2c;
  uint8_t addr_size = 1;
  if(!is_work_mode) {
    port = CST816->i2c_boot;
    addr_size = 2;
  }
  i2c_use(port);
  uint8_t regad[2] = { register_address >> 8, register_address & 0xFF };
  bool rv = i2c_write_block(port, addr_size, is_work_mode?regad+1:regad);
  if (rv) {
    rv = i2c_read_block(port, size, result);
  }
  i2c_release(port);
  mutex_unlock(s_i2c_lock);
  return rv;
}

static bool prv_write_data(uint16_t register_address, const uint8_t *datum, uint16_t size, bool is_work_mode) {
  mutex_lock(s_i2c_lock);
  I2CSlavePort* port = CST816->i2c;
  uint8_t addr_size = 1;
  if(!is_work_mode) {
    port = CST816->i2c_boot;
    addr_size = 2;
  }
  i2c_use(port);
  uint8_t data[size + sizeof(register_address)];
  data[0] = register_address >> 8;
  data[1] = register_address & 0xFF;
  memcpy(data+sizeof(register_address), datum, size);
  bool rv = i2c_write_block(port, size+addr_size, is_work_mode?data+1:data);
  i2c_release(port);
  mutex_unlock(s_i2c_lock);
  return rv;
}

static bool cst816_enter_bootmode(void) {
#if RESET_PIN_CTRLBY_NPM1300
  NPM1300_OPS.gpio_set(Npm1300_Gpio2, 0);
  psleep(CST816_RESET_CYCLE_TIME);
  NPM1300_OPS.gpio_set(Npm1300_Gpio2, 1);
  psleep(CST816_RESET_CYCLE_TIME);
#else
  gpio_output_set(&CST816->reset, true);
  psleep(CST816_RESET_CYCLE_TIME);
  gpio_output_set(&CST816->reset, false);
  psleep(CST816_RESET_CYCLE_TIME);
#endif

  uint8_t retry_cnt = 10;
  while (retry_cnt--) {
    uint8_t cmd = CST816_BOOT_MODE_CMD;
    bool rv = prv_write_data(CST816_BOOT_MODE_REG, &cmd, 1, 0);
    psleep(CST816_REG_WR_DELAY_TIME);
    rv &= prv_read_data(CST816_BOOT_FLAG_REG, &cmd, 1, 0);
    psleep(CST816_REG_WR_DELAY_TIME);

    if (cmd == CST816_BOOT_FLAG_VAL) {
      return true;
    }
  }

  return false;
}

static uint16_t cst816_read_checksum(void)
{
  uint8_t cmd = 0;
  bool rv = prv_write_data(CST816_BOOT_FLAG_REG, &cmd, 1, 0);
  psleep(CST816_FW_CHECKSUM_CAL_TIME);

  uint8_t data[2];
  rv &= prv_read_data(CST816_FW_CHECKSUM_REG, data, 2, 0);
  PBL_ASSERT(rv, "get checksum error");
  uint16_t checksum = (((uint16_t)(data[1] & 0xFF)) << 8) | data[0];

  return checksum;
}

static bool cst816_fw_update(void) {
  if (sizeof(app_bin) > 10) {
    uint16_t start_addr = (((uint16_t)(app_bin[1] & 0xFF)) << 8) | app_bin[0];
    uint16_t length = (((uint16_t)(app_bin[3] & 0xFF)) << 8) | app_bin[2];
    uint16_t checksum = (((uint16_t)(app_bin[5] & 0xFF)) << 8) | app_bin[4];
    uint16_t fw_offset = 6;
    
    while (length) {
      PBL_LOG_DBG("fw start_addr:%d length:%d", start_addr, length);
      uint8_t addr[2] = {start_addr&0xff, start_addr>>8};
      bool rv = prv_write_data(CST816_FW_START_ADDR_REG, addr, 2, 0);
      psleep(CST816_REG_WR_DELAY_TIME);
      if(!prv_write_data(CST816_FW_PAGE_REG, app_bin+fw_offset,
                        length>=CST816_FW_PAGE_SIZE?CST816_FW_PAGE_SIZE:length , 0)) {
        PBL_LOG_ERR("cst816 update fw error by iic");
        return false;
      }
      psleep(CST816_REG_WR_DELAY_TIME);
      uint8_t cmd = 0xEE;
      rv = prv_write_data(CST816_FW_PAGE_DONE, &cmd, 1, 0);
      psleep(CST816_FW_WR_TIME);
      for (int t=0;; t++) {
        if(t > 50) {
          PBL_LOG_ERR("cst816 update fw error by writing timeout");
          return false;
        }
        psleep(CST816_RESET_CYCLE_TIME);
        uint8_t ready;
        rv = prv_read_data(CST816_FW_PAGE_STATE, &ready, 1, 0);
        if (rv && ready == CST816_FW_PAGE_READY) {
          break;
        }
      }
      fw_offset += CST816_FW_PAGE_SIZE;
      start_addr += CST816_FW_PAGE_SIZE;
      length -= length>=CST816_FW_PAGE_SIZE?CST816_FW_PAGE_SIZE:length;
    }

    uint16_t checksum_read = cst816_read_checksum();
    if (checksum_read == checksum) {
      uint8_t boot_exit_cmd = CST816_BOOT_EXIT_VAL;
      bool rv = prv_write_data(CST816_BOOT_EXIT_REG, &boot_exit_cmd, 1, 0);
      if (!rv) {
        PBL_LOG_ERR("exit boot failed");
        return false;
      }

      PBL_LOG_INFO("Updated firmware to version 0x%02X (0x%04X)",
                   app_bin[sizeof(app_bin) + CST816_FW_VER_INFO_INDEX], checksum_read);

      cst816_hw_reset();
      return true;
    }
    PBL_LOG_ERR("cst816 update fw error by checksum:%x read:%x", checksum, checksum_read);
  }

  return false;
}

static void cst816_hw_reset(void) {
#ifdef RESET_PIN_CTRLBY_NPM1300
  NPM1300_OPS.gpio_set(Npm1300_Gpio2, 0);
  psleep(CST816_RESET_CYCLE_TIME);
  NPM1300_OPS.gpio_set(Npm1300_Gpio2, 1);
  psleep(CST816_POR_DELAY_TIME);
#else
  gpio_output_set(&CST816->reset, true);
  psleep(CST816_RESET_CYCLE_TIME);
  gpio_output_set(&CST816->reset, false);
  psleep(CST816_POR_DELAY_TIME);
#endif
}

void touch_sensor_init(void) {
  uint8_t chip_id;
  uint8_t fw_version;
  bool rv;

  s_i2c_lock = mutex_create();

#ifndef RESET_PIN_CTRLBY_NPM1300
  gpio_output_init(&CST816->reset, GPIO_OType_PP);
#endif

  cst816_hw_reset();

  rv = prv_read_data(CST816_CHIP_ID_REG, &chip_id, 1, 1);
  if (!rv) {
    PBL_LOG_ERR("Could not read CST816 chip ID");
    return;
  }

  rv = prv_read_data(CST816_FW_VERSION_REG, &fw_version, 1, 1);
  if (!rv) {
    PBL_LOG_ERR("Could not read CST816 firmware version");
    return;
  }

  PBL_LOG_DBG("CST816 firmware: 0x%02X", fw_version);

  uint8_t target_ver = app_bin[sizeof(app_bin) + CST816_FW_VER_INFO_INDEX];

  // Only ever write firmware to a chip we can positively identify as a CST816S/T.
  // An unrecognized chip ID means either a different part or a bad read; flashing
  // blindly based on fw_version alone could brick whatever is actually on the bus.
  bool chip_id_recognized =
      (chip_id == CST816_CHIP_ID_CST816S) || (chip_id == CST816_CHIP_ID_CST816T);

#ifdef CONFIG_SOC_NRF52
  // TEMPORARY bangle2 recovery: panel was mis-flashed with the getafix blob,
  // which self-reports chip ID 0xB5. Allow one recovery flash. Remove after
  // recovery (chip reports 0xB6 once CST816D fw is installed).
  chip_id_recognized = chip_id_recognized || (chip_id == 0xB5);
#endif

  if (target_ver != fw_version) {
    if (!chip_id_recognized) {
      PBL_LOG_WRN(
          "Unrecognized touch chip ID 0x%02X (fw 0x%02X != target 0x%02X); "
          "skipping touch-fw auto-update",
          chip_id, fw_version, target_ver);
    } else if (cst816_enter_bootmode()) {
      rv = cst816_fw_update();
      if (!rv) {
        return;
      }
    } else {
      PBL_LOG_ERR("Could not enter CST816 boot mode");
      return;
    }
  }

#ifdef CONFIG_SOC_NRF52
  // Deferred stroke-finalize timer for release-coalescing (recognizer rule 9).
  s_coalesce_timer = new_timer_create();
  PBL_ASSERTN(s_coalesce_timer != TIMER_INVALID_ID);

  // Continuous coordinate reporting (recognizer upgrade 4); logs on failure,
  // never fatal. Re-applied on every enable/recovery reset below.
  prv_config_irq_ctl();

  // The nRF exti driver (src/fw/drivers/exti/nrf5.c) expects the GPIO input
  // buffer to already be configured -- unlike SiFli's exti driver, it does not
  // configure the pin itself (see nrf5/button.c for the pattern other nRF exti
  // consumers follow). Without this, the INT pin's input buffer stays
  // disconnected (PIN_CNF reset default) and GPIOTE never observes the edge.
  // INT is push-pull, so NOPULL is correct.
  nrf_gpio_cfg_input(CST816->int_exti.gpio_pin, NRF_GPIO_PIN_NOPULL);
#endif

  // initialize exti
  exti_configure_pin(CST816->int_exti, ExtiTrigger_Falling, prv_exti_cb);

  touch_sensor_set_enabled(false);
}

static void prv_process_pending_messages(void* context) {
  bool rv;
  s_callback_scheduled = false;

  // Any interrupt means the chip is alive; pet the idle watchdog.
  s_activity_since_check = true;

  // Count interrupts spaced >=2s apart as sleep->awake transitions.
  RtcTicks now = rtc_get_ticks();
  if (now - s_last_irq_ticks >= milliseconds_to_ticks(CST816_WAKE_SPACING_MS)) {
    PBL_ANALYTICS_ADD(touch_driver_wake_cnt, 1);
  }
  s_last_irq_ticks = now;

  uint8_t id;
  rv = prv_read_data(CST816_GESTURE_ID, &id, 1, 1);
  if (!rv) {
    PBL_LOG_ERR("Failed to read gesture ID, trying to recover");
#ifdef CONFIG_SOC_NRF52
    prv_stroke_reset();
#endif
    /* (0,0) is an error-path sentinel, not a framebuffer position. */
    touch_handle_update(TouchState_FingerUp, 0, 0);
    exti_disable(CST816->int_exti);
    touch_sensor_set_enabled(true);
    return;
  }

  uint8_t data[CST816_TOUCH_DATA_SIZE] = {0};
  rv = prv_read_data(CST816_TOUCH_DATA_REG, data, CST816_TOUCH_DATA_SIZE, 1);
  if (!rv) {
    PBL_LOG_ERR("Failed to read touch data, trying to recover");
#ifdef CONFIG_SOC_NRF52
    prv_stroke_reset();
#endif
    /* (0,0) is an error-path sentinel, not a framebuffer position. */
    touch_handle_update(TouchState_FingerUp, 0, 0);
    exti_disable(CST816->int_exti);
    touch_sensor_set_enabled(true);
    return;
  }

  uint8_t press = data[0] & 0x0F;
  GPoint point = {
    .x = (((uint16_t)(data[1] & 0x0F)) << 8) | data[2],
    .y = (((uint16_t)(data[3] & 0X0F)) << 8) | data[4],
  };

  if (CST816->invert_x_axis) {
    point.x = CST816->max_x - point.x;
  }

  if (CST816->invert_y_axis) {
    point.y = CST816->max_y - point.y;
  }

#ifdef CONFIG_SOC_NRF52
  /* WORKAROUND (bangle2): trajectory-based recognizer; one gesture per
   * stroke, decided when the release-coalescing window closes. See the block
   * comment above the CST816_EDGE_CLAMP_X_LO definition for the measured
   * rationale and the physical->emitted mapping table. */

  /* Rule 7: XposH[7:6] event type (0=Down, 1=LiftUp, 2=Contact). */
  const uint8_t event = data[1] >> CST816_EVENT_SHIFT;

  /* Rules 7+8: LiftUp frames carry garbage x/y (lupyuen); frames with x
   * outside the panel are edge blips (Espruino). Neither may feed the
   * trajectory. bangle2 has no axis inversion, so point == raw chip coords. */
  const bool coord_valid = (event != CST816_EVENT_LIFTUP) &&
                           (point.x < CST816_COORD_X_LIMIT);
  /* Rule 7: a LiftUp event OR fingers==0 releases the stroke. */
  const bool is_release = (press == 0) || (event == CST816_EVENT_LIFTUP);
  bool coalesced = false;

  if (!is_release) {
    if (s_stroke_active && s_pending_release) {
      if ((now - s_release_ticks) <= milliseconds_to_ticks(CST816_COALESCE_MS)) {
        /* Rule 9: re-contact inside the window is the chip dropping and
         * re-grabbing one physical slide -- CONTINUE the stroke, keeping the
         * original down anchor and displacement history. */
        s_pending_release = false;
        new_timer_stop(s_coalesce_timer);
        coalesced = true;
      } else {
        /* Window long past but the deferred finalize hasn't run yet (system
         * task backlog): settle the old stroke before starting the new one. */
        prv_finalize_stroke();
      }
    }
    if (!s_stroke_active) {
      /* Finger down: start a new stroke. A mid-stroke fingers 1->2 ghost
       * never lands here -- the stroke is already active, so extra fingers
       * simply CONTINUE it (rule 5); only a release (rule 7) ends a stroke. */
      prv_stroke_reset();
      s_stroke_active = true;
      s_start_ticks = now;
    }
    s_frame_count++;

    if (coord_valid) {
      /* Edge-travel credit (rule 3): anchor at the stroke's FIRST valid
       * frame, edge-clamped or not, and track max |dx|+|dy| from it. Leading
       * x==238/0 frames carry real early travel for SWIPE qualification
       * only; the tap test keeps the strict trimmed anchor below. */
      if (!s_first_captured) {
        s_first_captured = true;
        s_first_point = point;
      } else {
        const int32_t fdx = point.x - s_first_point.x;
        const int32_t fdy = point.y - s_first_point.y;
        const int32_t fdisp = ABS(fdx) + ABS(fdy);
        if (fdisp > s_max_disp_first) {
          s_max_disp_first = fdisp;
          s_max_dx_first = (int16_t)fdx;
          s_max_dy_first = (int16_t)fdy;
        }
      }

      const bool x_clamped = (point.x <= CST816_EDGE_CLAMP_X_LO) ||
                             (point.x >= CST816_EDGE_CLAMP_X_HI);
      if (!x_clamped) {
        s_all_frames_clamped = false;
        s_last_unclamped_point = point;
        if (!s_down_captured) {
          /* Rule 1: anchor at the first NON-edge-clamped frame, skipping the
           * 2-4 pinned frames seen when the finger enters from the bezel. */
          s_down_captured = true;
          s_down_point = point;
          s_down_ticks = now;
        }
      }

      if (s_down_captured) {
        const int32_t dx = point.x - s_down_point.x;
        const int32_t dy = point.y - s_down_point.y;
        const int32_t disp = ABS(dx) + ABS(dy);
        if (disp > s_max_disp) {
          s_max_disp = disp;
          s_max_dx = (int16_t)dx;
          s_max_dy = (int16_t)dy;
        }
      }

      /* Last GOOD position; doubles as the release point (rule 7). */
      s_last_point = point;
    }

    /* Raw swipe code: recorded as a DIAGNOSTIC hint only. Measured
     * unreliable (identical swipes yielded 02/04/02); it never drives the
     * classification (rule in header comment). */
    if ((id >= CST816_GESTURE_RIGHT) && (id <= CST816_GESTURE_UP)) {
      s_stroke_code = id;
    }
  } else if (s_stroke_active && !s_pending_release) {
    /* Release: do NOT finalize yet -- arm the coalescing window (rule 9).
     * The stroke's classification end time is NOW even though dispatch is
     * deferred by up to CST816_COALESCE_MS. */
    s_pending_release = true;
    s_release_ticks = now;
    s_release_generation++;
    new_timer_start(s_coalesce_timer, CST816_COALESCE_MS, prv_coalesce_timer_cb,
                    (void *)(uintptr_t)s_release_generation, 0);
  }

  /* TEMPORARY bangle2 touch debug: record every processed frame. */
  {
    volatile Cst816TouchLog *slot = &s_cst816_touchlog[s_cst816_touchlog_count & 255];
    slot->tick = (uint32_t)now;
    slot->raw_gesture = id;
    slot->fingers = data[0];
    slot->x = (uint16_t)point.x;
    slot->y = (uint16_t)point.y;
    slot->action = 0xFF; /* dispatch decisions live in finalize entries (0xF0) */
    slot->evt = (uint8_t)(event | (coord_valid ? 0 : 0x10) | (coalesced ? 0x20 : 0));
    s_cst816_touchlog_count++;
  }

  /* Feed the touch service the last GOOD position when this frame's coords
   * are corrupt (LiftUp garbage / edge blip); wiring in touch.c unchanged. */
  GPoint report_point = coord_valid ? point : s_last_point;
  /* Leading-clamped-frame policy: a MID-stroke frame pinned at an x clamp
   * carries no position; substitute the last non-edge-clamped point. A
   * LEADING clamped frame (no down anchor yet) transforms its clamped
   * coordinate as-is -- blind substitution would hit-test at the PREVIOUS
   * stroke's endpoint (s_last_point survives prv_stroke_reset, and 7/14
   * measured strokes begin clamped). */
  if (((report_point.x <= CST816_EDGE_CLAMP_X_LO) || (report_point.x >= CST816_EDGE_CLAMP_X_HI)) &&
      s_down_captured) {
    report_point = s_last_unclamped_point;
  }
#ifdef CONFIG_BOARD_BANGLE2
  /* Chip -> framebuffer space (shared de-shear core + letterbox reversal);
   * gesture classification above stays in chip units. */
  report_point = GPoint(cst816_transform_fb_x(report_point.x, report_point.y),
                        cst816_transform_fb_y(report_point.x, report_point.y));
#endif
  if ((press == 0x01) && (event != CST816_EVENT_LIFTUP)) {
    touch_handle_update(TouchState_FingerDown, report_point.x, report_point.y);
  } else {
    touch_handle_update(TouchState_FingerUp, report_point.x, report_point.y);
  }
#else
  const int gesture = prv_decode_gesture(id);
  if (gesture >= 0) {
    touch_handle_gesture((TouchGesture)gesture, point.x, point.y);
  }

  if (press == 0x01) {
    touch_handle_update(TouchState_FingerDown, point.x, point.y);
  } else {
    touch_handle_update(TouchState_FingerUp, point.x, point.y);
  }
#endif
}

static void prv_exti_cb(bool *should_context_switch) {
  if (s_callback_scheduled) {
    return;
  }

  system_task_add_callback_from_isr(prv_process_pending_messages, NULL, should_context_switch);
  s_callback_scheduled = true;
}

// Runs on the system task: the actual recovery reset (cst816_hw_reset() sleeps
// ~120ms, so it must not run on the regular-timer task).
static void prv_idle_reset_worker(void *context) {
  s_reset_scheduled = false;
  if (!s_enabled) {
    return;
  }

  exti_disable(CST816->int_exti);
  cst816_hw_reset();
  s_callback_scheduled = false;
  s_activity_since_check = true;
#ifdef CONFIG_SOC_NRF52
  prv_stroke_reset();
  prv_config_irq_ctl();  // hard reset restored the blob's IrqCtl default
#endif
  exti_enable(CST816->int_exti);
}

// Runs on the regular-timer (NewTimers) task; keep it cheap, just offload.
static void prv_watchdog_cb(void *data) {
  if (!s_enabled || s_reset_scheduled) {
    return;
  }

  if (s_activity_since_check) {
    s_activity_since_check = false;
    return;
  }

  s_reset_scheduled = true;
  system_task_add_callback(prv_idle_reset_worker, NULL);
}

void touch_sensor_set_enabled(bool enabled) {
  cst816_hw_reset();

#ifdef CONFIG_SOC_NRF52
  prv_stroke_reset();
#endif

  if (enabled) {
#ifdef CONFIG_SOC_NRF52
    prv_config_irq_ctl();  // hard reset above restored the blob's IrqCtl default
#endif
    exti_enable(CST816->int_exti);
    s_enabled = true;
    s_activity_since_check = true;
    if (!regular_timer_is_scheduled(&s_watchdog_timer)) {
      regular_timer_add_multiminute_callback(&s_watchdog_timer, CST816_WATCHDOG_PERIOD_MIN);
    }
  } else {
    s_enabled = false;
    if (regular_timer_is_scheduled(&s_watchdog_timer)) {
      regular_timer_remove_callback(&s_watchdog_timer);
    }
    uint8_t data = CST816_POWER_MODE_SLEEP;
    prv_write_data(CST816_POWER_MODE_REG, &data, 1, 1);
    exti_disable(CST816->int_exti);
  }
}
