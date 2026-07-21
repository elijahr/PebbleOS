/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "board/board.h"
#include "drivers/flash/qspi_flash_definitions.h"
#include "drivers/gpio.h"
#include "drivers/pwm.h"
#include "drivers/qspi_definitions.h"
#include "drivers/rtc.h"
#include "drivers/uart/nrf5.h"
#include "system/logging.h"
#include "system/passert.h"

#include <hal/nrf_clock.h>
#include <hal/nrf_gpio.h>
#include <nrfx_gpiote.h>
#include <nrfx_pwm.h>
#include <nrfx_qspi.h>
#include <nrfx_spim.h>

// External flash (8 MB SPI NOR). The real Bangle.js 2 flash bus is software-SPI
// on GPIO (CS P0.14, SCK P0.16, IO0 P0.15, IO1 P0.13); Track A assigns the
// nRF52840 QSPI peripheral to those pins and picks the concrete opcode table.
// Present here so the flash driver links for the PRF skeleton.
static QSPIPortState s_qspi_port_state;
static QSPIPort QSPI_PORT = {
    .state = &s_qspi_port_state,
    .clk_freq_hz = 8000000UL,
    .cs_gpio = NRF_GPIO_PIN_MAP(0, 14),
    .clk_gpio = NRF_GPIO_PIN_MAP(0, 16),
    .data_gpio =
        {
            NRF_GPIO_PIN_MAP(0, 15),  // IO0 / MOSI
            NRF_GPIO_PIN_MAP(0, 13),  // IO1 / MISO
            NRF_GPIO_PIN_MAP(0, 20),  // IO2 (WP) - not wired on Bangle
            NRF_GPIO_PIN_MAP(0, 21),  // IO3 (HOLD) - not wired on Bangle
        },
};
QSPIPort *const QSPI = &QSPI_PORT;

static QSPIFlashState s_qspi_flash_state;
static QSPIFlash QSPI_FLASH_DEVICE = {
    .state = &s_qspi_flash_state,
    .qspi = &QSPI_PORT,
    // Only IO0 (P0.15) and IO1 (P0.13) are wired on the Bangle flash bus, so
    // reads use dual-IO and writes stay single-line (page program).
    .read_mode = QSPI_FLASH_READ_READ2IO,
    .write_mode = QSPI_FLASH_WRITE_PP,
};
QSPIFlash *const QSPI_FLASH = &QSPI_FLASH_DEVICE;

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

/* buttons */
IRQ_MAP_NRFX(TIMER1, nrfx_timer_1_irq_handler);
IRQ_MAP_NRFX(TIMER2, nrfx_timer_2_irq_handler);

/* EXTI */
IRQ_MAP_NRFX(GPIOTE, nrfx_gpiote_0_irq_handler);

/* backlight */
PwmState BACKLIGHT_PWM_STATE;
IRQ_MAP_NRFX(PWM0, nrfx_pwm_0_irq_handler);

IRQ_MAP_NRFX(RTC1, rtc_irq_handler);

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
}
