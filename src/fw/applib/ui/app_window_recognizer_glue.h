/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

struct Window;

////////////////////////////////////////////////
// App + Recognizer + Window : Glue code (impl plan section 12)
//
// Wires window lifecycle transitions (appear, off-screen, focus) and the
// per-app recognizer attach counter (Task 3) to the app task's
// RecognizerManager (Task 3) and its touch_service subscription. Mirrors
// app_window_click_glue.c's role for the click config provider.

#ifdef CONFIG_TOUCH

//! Called when the app task's recognizer-attach counter changes (Task 3).
//! Subscribes the recognizer manager to touch_service and the app-focus
//! event on the 0->1 transition; unsubscribes both on the 1->0 transition.
void app_window_recognizer_glue_attach_count_changed(uint16_t new_count);

//! Appear seam: called when a window's click config provider is (re)run
//! while the window is on screen. Set-before-cancel ordering (design
//! Section 6 point 5).
void app_window_recognizer_glue_window_focused(struct Window *window);

//! Off-screen choke point: called ONLY from window_set_on_screen's
//! off-screen branch. The single site that cancels, resets, and un-sets
//! the manager's window (design Section 6).
void app_window_recognizer_glue_window_off_screen(struct Window *window);

#else  // no-op stubs for every CONFIG_TOUCH=n consumer

static inline void app_window_recognizer_glue_attach_count_changed(uint16_t new_count) {}
static inline void app_window_recognizer_glue_window_focused(struct Window *window) {}
static inline void app_window_recognizer_glue_window_off_screen(struct Window *window) {}

#endif
