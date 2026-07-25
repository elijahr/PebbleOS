/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "board/board.h"
#include "drivers/gpio.h"
#include "drivers/i2c.h"
#include "drivers/i2c/definitions.h"
#include "drivers/i2c/nrf5.h"
#include "drivers/hrm.h"
#include "drivers/hrm/vc31.h"
#include "drivers/imu/kx023/kx023.h"
#include "drivers/mag.h"
#include "drivers/pressure.h"
#include "drivers/pwm.h"
#include "drivers/rtc.h"
#include "drivers/touch/cst816/touch_sensor_definitions.h"
#include "drivers/uart/nrf5.h"
#include "system/logging.h"
#include "system/passert.h"

#include <hal/nrf_clock.h>
#include <hal/nrf_gpio.h>
#include <nrfx_gpiote.h>
#include <nrfx_pwm.h>
#include <nrfx_spim.h>
#include <nrfx_twim.h>

// External flash (8 MB SPI NOR) is driven over the nRF52840 SPIM2 master with a
// GPIO chip-select on the real Bangle.js 2 flash pins (CS P0.14, SCK P0.16,
// MOSI/IO0 P0.15, MISO/IO1 P0.13). The bus config lives in BOARD_CONFIG_FLASH
// (board_bangle2.h); the driver is drivers/flash/spi_nor. SPIM2 runs in blocking
// mode, so its IRQ never fires, but map the vector defensively.
IRQ_MAP_NRFX(SPI2_SPIM2_SPIS2, nrfx_spim_2_irq_handler);

// Debug UART on the internal UATX (P1.11) / UARX (P1.10) test pads per
// gfwilliams/pebble-banglejs2 (GORDON-SOURCED, single-source: BANGLEJS2.py has
// no console-pin declaration; unverified on our hardware — first-boot triage
// step 1 IS the verification). Pulse console:
//   python tools/pulse_console.py -t /dev/ttyUSB0
static UARTDeviceState s_dbg_uart_state;
static UARTDevice DBG_UART_DEVICE = {
    .state = &s_dbg_uart_state,
    .tx_gpio = NRF_GPIO_PIN_MAP(1, 11),
    .rx_gpio = NRF_GPIO_PIN_MAP(1, 10),
    .rts_gpio = NRF_UARTE_PSEL_DISCONNECTED,
    .cts_gpio = NRF_UARTE_PSEL_DISCONNECTED,
    .periph = NRFX_UARTE_INSTANCE(0),
    .counter = NRFX_TIMER_INSTANCE(2),
};
UARTDevice *const DBG_UART = &DBG_UART_DEVICE;
IRQ_MAP_NRFX(UART0_UARTE0, nrfx_uarte_0_irq_handler);

/* display (LPM013M126 on SPIM3 via interrupt-driven nrfx_spim EasyDMA) */
IRQ_MAP_NRFX(SPIM3, nrfx_spim_3_irq_handler);

/* buttons */
IRQ_MAP_NRFX(TIMER1, nrfx_timer_1_irq_handler);
IRQ_MAP_NRFX(TIMER2, nrfx_timer_2_irq_handler);

/* EXTI */
IRQ_MAP_NRFX(GPIOTE, nrfx_gpiote_0_irq_handler);

/* backlight */
PwmState BACKLIGHT_PWM_STATE;
IRQ_MAP_NRFX(PWM0, nrfx_pwm_0_irq_handler);

IRQ_MAP_NRFX(RTC1, rtc_irq_handler);

// --- Track B: input sensors (I2C on the two hardware TWIM controllers) --------
// Espruino "Dnn" pin numbers 32..47 are nRF52840 P1.00..P1.15, so the Bangle
// touch/accel pins (D33..D38) live on GPIO port 1, not port 0.

// Touch bus: CST816S on TWIM0. SDA D33=P1.01, SCL D34=P1.02.
static I2CBusState s_i2c_touch_bus_state = {};
static const I2CBusHal s_i2c_touch_bus_hal = {
    .twim = NRFX_TWIM_INSTANCE(0),
    .frequency = NRF_TWIM_FREQ_400K,
};
static const I2CBus s_i2c_touch_bus = {
    .state = &s_i2c_touch_bus_state,
    .hal = &s_i2c_touch_bus_hal,
    .scl_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = NRF_GPIO_PIN_MAP(1, 2)},
    .sda_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = NRF_GPIO_PIN_MAP(1, 1)},
    .name = "I2C_TOUCH",
};
I2CBus *const I2C_TOUCH_BUS = &s_i2c_touch_bus;
IRQ_MAP_NRFX(SPI0_SPIM0_SPIS0_TWI0_TWIM0_TWIS0, nrfx_twim_0_irq_handler);

// CST816S work-mode (0x15) and boot-mode (0x6A) slaves. nRF stores the 8-bit
// address (driver shifts right by one), matching board_asterix.c.
static const I2CSlavePort s_i2c_cst816 = {
    .bus = &s_i2c_touch_bus,
    .address = 0x15 << 1,
};
static const I2CSlavePort s_i2c_cst816_boot = {
    .bus = &s_i2c_touch_bus,
    .address = 0x6A << 1,
};

static const TouchSensor s_touch_cst816 = {
    .i2c = &s_i2c_cst816,
    .i2c_boot = &s_i2c_cst816_boot,
    // INT = D36 = P1.04 (falling-edge EXTI via GPIOTE ch 1).
    .int_exti =
        {
            .peripheral = NRFX_GPIOTE_INSTANCE(0),
            .channel = 1,
            .gpio_pin = NRF_GPIO_PIN_MAP(1, 4),
        },
    // RST = D35 = P1.03, active low.
    .reset =
        {
            .gpio = NRF5_GPIO_RESOURCE_EXISTS,
            .gpio_pin = NRF_GPIO_PIN_MAP(1, 3),
            .active_high = false,
        },
    .max_x = 175,
    .max_y = 175,
    .invert_x_axis = false,
    .invert_y_axis = false,
};
const TouchSensor *CST816 = &s_touch_cst816;

// Accel bus: KX023 on TWIM1. SDA D38=P1.06, SCL D37=P1.05. No interrupt wired.
static I2CBusState s_i2c_accel_bus_state = {};
static const I2CBusHal s_i2c_accel_bus_hal = {
    .twim = NRFX_TWIM_INSTANCE(1),
    .frequency = NRF_TWIM_FREQ_400K,
};
static const I2CBus s_i2c_accel_bus = {
    .state = &s_i2c_accel_bus_state,
    .hal = &s_i2c_accel_bus_hal,
    .scl_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = NRF_GPIO_PIN_MAP(1, 5)},
    .sda_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = NRF_GPIO_PIN_MAP(1, 6)},
    .name = "I2C_ACCEL",
};
I2CBus *const I2C_ACCEL_BUS = &s_i2c_accel_bus;
IRQ_MAP_NRFX(SPI1_SPIM1_SPIS1_TWI1_TWIM1_TWIS1, nrfx_twim_1_irq_handler);

static KX023State s_kx023_state;
static const KX023Config s_kx023_config = {
    .state = &s_kx023_state,
    .i2c =
        {
            .bus = &s_i2c_accel_bus,
            .address = 0x1E << 1,
        },
    .axis_map = {[AXIS_X] = 0, [AXIS_Y] = 1, [AXIS_Z] = 2},
    .axis_dir = {[AXIS_X] = 1, [AXIS_Y] = 1, [AXIS_Z] = 1},
};
const KX023Config *const KX023 = &s_kx023_config;

// --- Stage 2: HRM (VC31) on the repurposed Stage-1 software (bit-bang) bus -----
// The real Bangle.js 2 heart-rate/mag/pressure sensors sit on bit-banged GPIO
// I2C buses because all four nRF52840 hardware serial instances are already used
// (display SPIM3, flash SPIM2, touch TWIM0, accel TWIM1). Stage 1 stood up the
// bit-bang foundation on the spare HRM GPIOs with an echo test slave; Stage 2
// repurposes that bus into the REAL HRM bus: a Vcare VC31/VC31B at I2C 0x33
// (Espruino boards/BANGLEJS2.py). Real Bangle.js 2 wiring is SDA D24=P0.24, SCL
// D32=P1.00 (absolute pin 32), EN D21=P0.21, INT D22=P0.22 — note the SDA/SCL
// assignment is the reverse of the provisional Stage-1 test bus, corrected here.
//
// VC31 is INTERFACE-ONLY: the PPG->BPM algorithm is a proprietary blob, so the
// driver only powers the part and reads raw registers (drivers/hrm/vc31.c). Two
// firmware self-tests exercise the bus: prv_soft_i2c_selftest reads the VC31
// WHO_AM_I over the RAW bit-bang i2c API (foundation proof, soft_i2c harness),
// and prv_hrm_selftest drives the VC31 driver through hrm.h (hrm harness).
#define HRM_SDA_PIN NRF_GPIO_PIN_MAP(0, 24)  // D24
#define HRM_SCL_PIN NRF_GPIO_PIN_MAP(1, 0)   // D32 (absolute pin 32)
#define HRM_EN_PIN NRF_GPIO_PIN_MAP(0, 21)   // D21
#define HRM_INT_PIN NRF_GPIO_PIN_MAP(0, 22)  // D22
#define VC31_I2C_ADDR 0x33
#define VC31_REG_WHO_AM_I 0x00

static I2CBusState s_i2c_hrm_bus_state = {};
static const I2CBusHal s_i2c_hrm_bus_hal = {
    .type = I2CBusHalType_BitBang,
    .bitbang_half_period_us = 2,
};
static const I2CBus s_i2c_hrm_bus = {
    .state = &s_i2c_hrm_bus_state,
    .hal = &s_i2c_hrm_bus_hal,
    .scl_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = HRM_SCL_PIN},
    .sda_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = HRM_SDA_PIN},
    .name = "I2C_HRM",
};
I2CBus *const I2C_HRM_BUS = &s_i2c_hrm_bus;

// 8-bit address stored; the driver shifts right by one for the wire address.
static const I2CSlavePort s_i2c_vc31 = {
    .bus = &s_i2c_hrm_bus,
    .address = VC31_I2C_ADDR << 1,
};

static HRMDeviceState s_hrm_state;
static HRMDevice s_hrm = {
    .state = &s_hrm_state,
    .i2c = &s_i2c_vc31,
    .en_gpio = {NRF5_GPIO_RESOURCE_EXISTS, HRM_EN_PIN, true},
    .int_input = {NRF5_GPIO_RESOURCE_EXISTS, HRM_INT_PIN},
};
HRMDevice *const HRM = &s_hrm;

// Foundation self-test: read the VC31 WHO_AM_I over the RAW bit-bang i2c API
// (unchanged symbols so the soft_i2c harness keeps proving the bit-bang path,
// now against the real HRM sensor instead of the retired echo slave). Volatile
// so the compiler cannot fold the read away.
volatile uint8_t g_soft_i2c_test_ran = 0;
volatile uint8_t g_soft_i2c_test_ok = 0;
volatile uint8_t g_soft_i2c_test_value = 0;
volatile uint8_t g_soft_i2c_test_reg = VC31_REG_WHO_AM_I;

static void prv_soft_i2c_selftest(void) {
  i2c_use(&s_i2c_vc31);
  uint8_t value = 0;
  bool ok = i2c_read_register(&s_i2c_vc31, VC31_REG_WHO_AM_I, &value);
  i2c_release(&s_i2c_vc31);

  g_soft_i2c_test_value = value;
  g_soft_i2c_test_ok = ok ? 1 : 0;
  g_soft_i2c_test_ran = 1;

  if (ok) {
    PBL_LOG_INFO("soft I2C self-test: VC31 WHO_AM_I = 0x%02x", value);
  } else {
    PBL_LOG_ERR("soft I2C self-test FAILED (no ACK / wrong wiring)");
  }
}

// HRM driver self-test: bring the VC31 up through the hrm.h interface and record
// the result for the hrm harness. INTERFACE-ONLY: no heart rate is computed.
volatile uint8_t g_hrm_test_ran = 0;
volatile uint8_t g_hrm_test_ok = 0;
volatile uint8_t g_hrm_test_whoami = 0;

static void prv_hrm_selftest(void) {
  hrm_init(HRM);
  bool en = hrm_enable(HRM);

  g_hrm_test_whoami = s_hrm_state.chip_id;
  g_hrm_test_ok = (s_hrm_state.initialized && en) ? 1 : 0;
  g_hrm_test_ran = 1;

  hrm_disable(HRM);

  if (g_hrm_test_ok) {
    PBL_LOG_INFO("HRM (VC31) self-test: id 0x%02x, interface-only", s_hrm_state.chip_id);
  } else {
    PBL_LOG_ERR("HRM (VC31) self-test FAILED (probe error)");
  }
}

// --- Stage 2: pressure sensor (BMP280) on a software (bit-bang) I2C bus -------
// The Bangle.js 2 barometer is a Bosch BMP280 at I2C 0x76 on its own bit-banged
// GPIO pair (Espruino boards/BANGLEJS2.py: SDA D47=P1.15, SCL D2=P0.02). All
// four nRF52840 serial instances are already used, so this is a software bus.
#define BMP280_SDA_PIN NRF_GPIO_PIN_MAP(1, 15)  // D47
#define BMP280_SCL_PIN NRF_GPIO_PIN_MAP(0, 2)   // D2

static I2CBusState s_i2c_pressure_bus_state = {};
static const I2CBusHal s_i2c_pressure_bus_hal = {
    .type = I2CBusHalType_BitBang,
    .bitbang_half_period_us = 2,
};
static const I2CBus s_i2c_pressure_bus = {
    .state = &s_i2c_pressure_bus_state,
    .hal = &s_i2c_pressure_bus_hal,
    .scl_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = BMP280_SCL_PIN},
    .sda_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = BMP280_SDA_PIN},
    .name = "I2C_PRESSURE",
};
I2CBus *const I2C_PRESSURE_BUS = &s_i2c_pressure_bus;

// 8-bit address stored; the driver shifts right by one for the wire address.
static const I2CSlavePort s_i2c_bmp280 = {
    .bus = &s_i2c_pressure_bus,
    .address = 0x76 << 1,
};
I2CSlavePort *const I2C_BMP280 = &s_i2c_bmp280;

// Self-test results, read back from RAM by the pressure harness (resolved from
// the ELF). The compensated values prove the BMP280 register map + Bosch
// integer compensation round-trip THROUGH the bit-bang driver and the decoder.
volatile uint8_t g_pressure_test_ran = 0;
volatile uint8_t g_pressure_test_ok = 0;
volatile int32_t g_pressure_test_temp = 0;      // centidegrees C
volatile int32_t g_pressure_test_pressure = 0;  // pascals

static void prv_pressure_selftest(void) {
  pressure_init();
  int32_t p = 0;
  int32_t t = 0;
  bool ok = pressure_read(&p, &t);

  g_pressure_test_pressure = p;
  g_pressure_test_temp = t;
  g_pressure_test_ok = ok ? 1 : 0;
  g_pressure_test_ran = 1;

  if (ok) {
    PBL_LOG_INFO("BMP280 self-test: %ld Pa, %ld.%02ld C", (long)p, (long)(t / 100),
                 (long)(t % 100));
  } else {
    PBL_LOG_ERR("BMP280 self-test FAILED (probe / read error)");
  }
}

// --- Stage 2: magnetometer (UNKNOWN_0C) on a software (bit-bang) I2C bus ------
// The Bangle.js 2 compass is an UNIDENTIFIED part at I2C 0x0C (Espruino names it
// UNKNOWN_0C). Reverse-engineered access only. Pins (Espruino BANGLEJS2.py):
// SDA D44=P1.12, SCL D45=P1.13. Own bit-bang bus (all hardware serials in use).
#define MAG_SDA_PIN NRF_GPIO_PIN_MAP(1, 12)  // D44
#define MAG_SCL_PIN NRF_GPIO_PIN_MAP(1, 13)  // D45

static I2CBusState s_i2c_mag_bus_state = {};
static const I2CBusHal s_i2c_mag_bus_hal = {
    .type = I2CBusHalType_BitBang,
    .bitbang_half_period_us = 2,
};
static const I2CBus s_i2c_mag_bus = {
    .state = &s_i2c_mag_bus_state,
    .hal = &s_i2c_mag_bus_hal,
    .scl_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = MAG_SCL_PIN},
    .sda_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = MAG_SDA_PIN},
    .name = "I2C_MAG",
};
I2CBus *const I2C_MAG_BUS = &s_i2c_mag_bus;

// 8-bit address stored; the driver shifts right by one for the wire address.
static const I2CSlavePort s_i2c_mag = {
    .bus = &s_i2c_mag_bus,
    .address = 0x0C << 1,
};
I2CSlavePort *const I2C_MAG = &s_i2c_mag;

// Self-test results, read back from RAM by the mag harness. The raw XYZ prove
// the reverse-engineered register block round-trips THROUGH the bit-bang driver
// and the decoder into the mag driver's MagData.
volatile uint8_t g_mag_test_ran = 0;
volatile uint8_t g_mag_test_ok = 0;
volatile int16_t g_mag_test_x = 0;
volatile int16_t g_mag_test_y = 0;
volatile int16_t g_mag_test_z = 0;

static void prv_mag_selftest(void) {
  mag_init();
  mag_use();
  MagData d = {0};
  MagReadStatus st = mag_read_data(&d);
  mag_release();

  g_mag_test_x = d.x;
  g_mag_test_y = d.y;
  g_mag_test_z = d.z;
  g_mag_test_ok = (st == MagReadSuccess) ? 1 : 0;
  g_mag_test_ran = 1;

  if (st == MagReadSuccess) {
    PBL_LOG_INFO("mag self-test: (%d,%d,%d)", d.x, d.y, d.z);
  } else {
    PBL_LOG_ERR("mag self-test FAILED (status %d)", st);
  }
}

void board_early_init(void) {
  PBL_LOG_ERR("bangle2 early init");

  NRF_NVMC->ICACHECNF |= NVMC_ICACHECNF_CACHEEN_Msk;

  nrf_clock_lf_src_set(NRF_CLOCK, NRF_CLOCK_LFCLK_XTAL);
  nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED);
  nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_LFCLKSTART);
  /* TODO: Add timeout, report failure if LFCLK does not start. For now,
   * WDT should trigger a reboot. */
  while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED)) {
  }
  nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_LFCLKSTARTED);
}

void board_init(void) {
  i2c_init(I2C_TOUCH_BUS);
  i2c_init(I2C_ACCEL_BUS);
  i2c_init(I2C_HRM_BUS);
  prv_soft_i2c_selftest();
  prv_hrm_selftest();
  i2c_init(I2C_PRESSURE_BUS);
  prv_pressure_selftest();
  i2c_init(I2C_MAG_BUS);
  prv_mag_selftest();
}
