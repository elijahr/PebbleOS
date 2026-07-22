/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "board/board.h"
#include "drivers/gpio.h"
#include "drivers/i2c.h"
#include "drivers/i2c/definitions.h"
#include "drivers/i2c/nrf5.h"
#include "drivers/imu/kx023/kx023.h"
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

// Debug UART on unused GPIOs (placeholder pins; Bangle.js 2 has no dedicated
// debug UART routed in the Espruino source).
static UARTDeviceState s_dbg_uart_state;
static UARTDevice DBG_UART_DEVICE = {
    .state = &s_dbg_uart_state,
    .tx_gpio = NRF_GPIO_PIN_MAP(0, 9),
    .rx_gpio = NRF_GPIO_PIN_MAP(0, 10),
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

// --- Stage 1: software (bit-bang) I2C foundation ------------------------------
// The real Bangle.js 2 heart-rate/mag/pressure sensors sit on bit-banged GPIO
// I2C buses because all four nRF52840 hardware serial instances are already
// used (display SPIM3, flash SPIM2, touch TWIM0, accel TWIM1). This declares a
// SOFTWARE I2C bus on the spare HRM GPIOs (SCL P0.24, SDA absolute pin 32 =
// P1.00, the "P0.32" HRM line) so the foundation can be proven end-to-end
// before any real sensor driver is added. A boot-time self-test reads one known
// register from a test slave over the bit-bang driver; the banglejs2-renode
// soft_i2c harness asserts the value round-tripped through the GPIO->I2C
// decoder and the slave.
#define SOFT_I2C_SCL_PIN NRF_GPIO_PIN_MAP(0, 24)
#define SOFT_I2C_SDA_PIN NRF_GPIO_PIN_MAP(1, 0)  // absolute pin 32 (HRM "P0.32")
#define SOFT_I2C_SLAVE_ADDR 0x50
#define SOFT_I2C_TEST_REG 0x42

static I2CBusState s_i2c_soft_bus_state = {};
static const I2CBusHal s_i2c_soft_bus_hal = {
    .type = I2CBusHalType_BitBang,
    .bitbang_half_period_us = 2,
};
static const I2CBus s_i2c_soft_bus = {
    .state = &s_i2c_soft_bus_state,
    .hal = &s_i2c_soft_bus_hal,
    .scl_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = SOFT_I2C_SCL_PIN},
    .sda_gpio = {.gpio = NRF5_GPIO_RESOURCE_EXISTS, .gpio_pin = SOFT_I2C_SDA_PIN},
    .name = "I2C_SOFT",
};
I2CBus *const I2C_SOFT_BUS = &s_i2c_soft_bus;

static const I2CSlavePort s_i2c_soft_slave = {
    .bus = &s_i2c_soft_bus,
    // 8-bit address stored; the driver shifts right by one for the wire address.
    .address = SOFT_I2C_SLAVE_ADDR << 1,
};

// Self-test results, read back from RAM by the soft_i2c harness (resolved from
// the ELF). Volatile so the compiler cannot fold the read away.
volatile uint8_t g_soft_i2c_test_ran = 0;
volatile uint8_t g_soft_i2c_test_ok = 0;
volatile uint8_t g_soft_i2c_test_value = 0;
volatile uint8_t g_soft_i2c_test_reg = SOFT_I2C_TEST_REG;

static void prv_soft_i2c_selftest(void) {
  i2c_use(&s_i2c_soft_slave);
  uint8_t value = 0;
  bool ok = i2c_read_register(&s_i2c_soft_slave, SOFT_I2C_TEST_REG, &value);
  i2c_release(&s_i2c_soft_slave);

  g_soft_i2c_test_value = value;
  g_soft_i2c_test_ok = ok ? 1 : 0;
  g_soft_i2c_test_ran = 1;

  if (ok) {
    PBL_LOG_INFO("soft I2C self-test: reg 0x%02x = 0x%02x", SOFT_I2C_TEST_REG, value);
  } else {
    PBL_LOG_ERR("soft I2C self-test FAILED (no ACK / wrong wiring)");
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
  i2c_init(I2C_SOFT_BUS);
  prv_soft_i2c_selftest();
}
