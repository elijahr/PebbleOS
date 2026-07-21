/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// Direct-SAADC battery driver for the Bangle.js 2 (nRF52840, no PMIC).
//
// Battery voltage is sensed on SAADC AIN1 (P0.03); charge presence is the
// P0.23 GPIO (active LOW). Millivolts use the Espruino BANGLEJS2 calibration:
// the 12-bit SAADC reads ~0.3144 of full scale at 4.2 V, i.e. raw code 1288
// corresponds to 4200 mV, so millivolts = raw * 4200 / 1288. Percentage is
// derived downstream by the voltage battery service via the discharge curve.

#include "drivers/battery.h"
#include "drivers/gpio.h"

#include "board/board.h"
#include "system/logging.h"

#include <hal/nrf_gpio.h>
#include <hal/nrf_saadc.h>

// AIN1 = P0.03 battery sense.
#define BATTERY_SAADC_CHANNEL 0
#define BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN1

// Charge detect: P0.23, active low (line idles high = no charger).
#define CHARGE_DETECT_PIN NRF_GPIO_PIN_MAP(0, 23)

// Espruino BANGLEJS2 calibration: raw code 1288 <-> 4200 mV.
#define BATTERY_RAW_AT_4V2 1288
#define BATTERY_MV_AT_4V2 4200

// Poll guard so a mis-wired SAADC can never wedge the caller. The Renode model
// completes synchronously, so on emulation the events are already set.
#define SAADC_POLL_GUARD 1000000U

// Non-static so the Renode harness can peek the last conversion for a hard
// SAADC -> millivolts assertion (read via `sysbus ReadDoubleWord @<symbol>`).
volatile int32_t g_bangle2_battery_raw_code;
volatile int32_t g_bangle2_battery_mv;

void battery_init(void) {
  // Charge-detect input, pulled up so an open line reads high (not charging).
  nrf_gpio_cfg_input(CHARGE_DETECT_PIN, NRF_GPIO_PIN_PULLUP);

  nrf_saadc_resolution_set(NRF_SAADC, NRF_SAADC_RESOLUTION_12BIT);

  const nrf_saadc_channel_config_t config = {
    .gain = NRF_SAADC_GAIN1_6,
    .reference = NRF_SAADC_REFERENCE_INTERNAL,
    .acq_time = NRF_SAADC_ACQTIME_10US,
    .mode = NRF_SAADC_MODE_SINGLE_ENDED,
    .burst = NRF_SAADC_BURST_DISABLED,
  };
  nrf_saadc_channel_init(NRF_SAADC, BATTERY_SAADC_CHANNEL, &config);
  nrf_saadc_channel_input_set(NRF_SAADC, BATTERY_SAADC_CHANNEL, BATTERY_SAADC_INPUT,
                              NRF_SAADC_INPUT_DISABLED);
  nrf_saadc_enable(NRF_SAADC);
}

static uint16_t prv_sample_raw(void) {
  // DMA target: the SAADC writes the sample into RAM by EasyDMA, so it must be
  // volatile or the compiler returns the stale (zero) initial value.
  volatile int16_t sample = 0;
  volatile uint32_t guard;

  nrf_saadc_buffer_init(NRF_SAADC, (nrf_saadc_value_t *)&sample, 1);

  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STARTED);
  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_END);
  nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STOPPED);

  nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_START);
  for (guard = SAADC_POLL_GUARD;
       guard && !nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_STARTED); guard--) {
  }

  nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_SAMPLE);
  for (guard = SAADC_POLL_GUARD;
       guard && !nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_END); guard--) {
  }

  nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_STOP);
  for (guard = SAADC_POLL_GUARD;
       guard && !nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_STOPPED); guard--) {
  }

  // Single-ended reads are non-negative; clamp any offset noise to 0.
  if (sample < 0) {
    sample = 0;
  }
  return (uint16_t)sample;
}

int battery_get_millivolts(void) {
  const uint16_t raw = prv_sample_raw();
  const int32_t mv = (int32_t)raw * BATTERY_MV_AT_4V2 / BATTERY_RAW_AT_4V2;

  g_bangle2_battery_raw_code = raw;
  g_bangle2_battery_mv = mv;
  return mv;
}

int battery_get_constants(BatteryConstants *constants) {
  constants->v_mv = battery_get_millivolts();
  constants->i_ua = 0;
  constants->t_mc = 25000;
  return 0;
}

int battery_charge_status_get(BatteryChargeStatus *status) {
  *status = BatteryChargeStatusUnknown;
  return 0;
}

bool battery_charge_controller_thinks_we_are_charging_impl(void) {
  // Active low: pin low => charger present.
  return nrf_gpio_pin_read(CHARGE_DETECT_PIN) == 0U;
}

bool battery_is_usb_connected_impl(void) {
  return battery_charge_controller_thinks_we_are_charging_impl();
}

void battery_set_charge_enable(bool charging_enabled) {
  // Charge control is handled by the standalone charger IC; nothing to gate.
}

void battery_set_fast_charge(bool fast_charge_enabled) {
}
