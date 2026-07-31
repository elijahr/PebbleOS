/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "recognizer.h"

#include <stdint.h>

//! Minimum cumulative vertical travel, in delivered-coordinate px
//! (framebuffer px once the delivery transform lands), before a drag starts.
#define DRAG_START_THRESHOLD_PX 8

//! Minimum number of position updates before a drag starts.
#define DRAG_MIN_UPDATES 2

//! Horizontal-dominance ratio at which the drag fails instead of starting.
//! Uses the same constant and inclusivity as the cst816 driver's >=3:1
//! horizontal-dominance gate (prv_phys_from_deshear), so the two gates
//! resolve the shared 3:1 boundary consistently once they see the same
//! coordinate frame. NOTE: the driver evaluates its ratio on de-sheared
//! PHYSICAL components, while this recognizer sees delivered coordinates
//! -- framebuffer coordinates on bangle2 since the driver-side de-shear
//! landed -- so the two gates now share a coordinate frame there. Even
//! in a shared frame the driver keeps its own dead zone (displacement
//! between the tap ceiling and CST816_SWIPE_MIN_DISP, or fewer than
//! CST816_SWIPE_MIN_FRAMES frames,
//! dispatches nothing), so a stroke that fails this drag may still not
//! swipe. Silicon-tunable: validated together with the driver's ratio
//! during on-silicon bring-up validation.
#define DRAG_HORIZONTAL_FAIL_RATIO 3

//! Create a vertical drag recognizer. The drag starts once at least
//! DRAG_MIN_UPDATES position updates have accumulated at least
//! DRAG_START_THRESHOLD_PX of cumulative vertical travel; a stroke whose
//! |cum_dx| >= DRAG_HORIZONTAL_FAIL_RATIO * |cum_dy| fails instead so it
//! falls through to swipe emulation.
//! @param event_cb event callback
//! @param user_data user data associated with recognizer
//! @return recognizer reference
Recognizer *drag_recognizer_create(RecognizerEventCb event_cb, void *user_data);

//! Get the vertical delta delivered with the current event. Valid inside the
//! event callback: Started delivers all travel accumulated so far minus the
//! signed DRAG_START_THRESHOLD_PX (the threshold is absorbed as slop, so
//! nothing else is lost even when a sparse stroke packs large deltas into
//! the pre-start updates); Updated delivers the triggering event's dy;
//! Completed and Cancelled deliver 0 (liftoff coordinates are never read,
//! and a cancelled drag must not scroll).
//! @param recognizer recognizer from which to get the delta
//! @return per-event dy in delivered-coordinate px (framebuffer px once the
//! delivery transform lands)
int16_t drag_recognizer_get_delta_y(const Recognizer *recognizer);
