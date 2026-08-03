/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "touch_event.h"
#include "gesture_event.h"

#include <stdbool.h>

typedef enum TouchState {
  TouchState_FingerUp,
  TouchState_FingerDown,
} TouchState;

typedef enum TouchGesture {
  TouchGesture_Tap,
  TouchGesture_DoubleTap,
  //! Vertical swipes. The CST816 reports these natively; they are used by the
  //! touch-to-button navigation shim (CONFIG_TOUCH_NAV_BUTTONS) to scroll
  //! button-driven menus on devices with a touchscreen but no UP/DOWN buttons.
  TouchGesture_SwipeUp,
  TouchGesture_SwipeDown,
  //! Horizontal swipe. The CST816 reports it natively; the touch-to-button
  //! navigation shim (CONFIG_TOUCH_NAV_BUTTONS) maps swipe-right to the BACK
  //! button so drilled-in menus can be backed out of on devices with a
  //! touchscreen but no BACK button.
  TouchGesture_SwipeRight,
  //! Horizontal swipe. Decoded by the driver but intentionally not wired to
  //! any button by the touch-to-button navigation shim.
  TouchGesture_SwipeLeft,
} TouchGesture;

void touch_init(void);

//! Enable or disable the kernel's touch subscription used for the touch backlight feature.
//! When disabled, the touch sensor is only active if apps have subscribed to touch events.
void touch_set_backlight_enabled(bool enabled);

//! @return true if at least one subscriber is currently registered for touch events.
bool touch_has_app_subscribers(void);

//! Globally enable or disable touch. When disabled:
//! - The sensor is powered down, even if subscribers exist.
//! - touch_handle_update() drops incoming events at the source.
//! - touch_service_is_enabled() returns false to apps.
//! Subscribers remain subscribed and resume receiving events when re-enabled.
//! Intended to back a user-facing setting (e.g. "water mode") — the shell
//! pref system persists the value and calls this on boot.
void touch_service_set_globally_enabled(bool enabled);

//! @return the current value of the global touch-enabled flag.
bool touch_service_is_globally_enabled(void);

//! Pass a touch update to the service (called by the touch driver)
//! @param touch_state whether or not the screen is touched
//! @param x x position of touch
//! @param y y position of touch
void touch_handle_update(TouchState touch_state, int16_t x, int16_t y);

//! Handle a gesture update (called by the touch driver)
//! @param gesture gesture that was detected
//! @param x x position of gesture (if applicable)
//! @param y y position of gesture
void touch_handle_gesture(TouchGesture gesture, int16_t x, int16_t y);

//! Reset the touch service.
void touch_reset(void);

//! Set whether the display is rotated 180° (left-hand mode). When rotated,
//! incoming touch coordinates are mirrored to match the rotated framebuffer
//! before being dispatched to subscribers.
void touch_set_rotated(bool rotated);
