#include "drivers/battery.h"

void battery_init(void) {
}

bool battery_is_present(void) {
  return true;
}

int battery_get_millivolts(void) {
  return 4000;
}

int battery_get_constants(BatteryConstants *constants) {
  constants->v_mv = 4000;
  constants->i_ua = 100;
  constants->t_mc = 25000;
  return 0;
}

int battery_charge_status_get(BatteryChargeStatus *status) {
  *status = BatteryChargeStatusUnknown;
  return 0;
}

// Emulation-friendly defaults: report NOT charging and NO USB so the normal
// build auto-launches the watchface instead of parking on the Charging modal
// forever. battery_get_millivolts() reports a fixed ~mid-high charge (4000 mV),
// which keeps the watch out of the low-power path. Real battery/charge sensing
// is Track C.
bool battery_charge_controller_thinks_we_are_charging_impl(void) {
  return false;
}

bool battery_is_usb_connected_impl(void) {
  return false;
}

void battery_set_charge_enable(bool charging_enabled) {
}

void battery_set_fast_charge(bool fast_charge_enabled) {
}
