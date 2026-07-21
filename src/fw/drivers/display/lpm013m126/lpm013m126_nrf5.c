/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

// Driver for the Bangle.js 2 LPM013M126 memory-in-pixel LCD on the nRF52840
// SPIM3 bus.
//
// The system UI renders at the FLINT 144x168 1bpp mono geometry
// (PBL_DISPLAY_WIDTH x PBL_DISPLAY_HEIGHT). This driver letterboxes that
// framebuffer, centered by (LETTERBOX_OFFSET_X, LETTERBOX_OFFSET_Y), onto the
// physical 176x176 panel, expands each mono pixel to the panel's native 3bpp
// (white 0b111 / black 0b000), packs each panel line LSB-first, and prefixes it
// with the 2-byte line header the panel expects.
//
// Line/header/bit-order encoding follows Espruino's reference driver
// libs/graphics/lcd_memlcd.c (the ground truth for this panel):
//   - SPI 4 MHz, LSB-first.
//   - Per line: byte0 = reverseByte(0x80) (3bpp update command),
//               byte1 = reverseByte(y + 1) (1-based line address),
//               then LCD_LINE_DATA_BYTES of 3bpp pixels, LSB-first.
//   - CS is a GPIO, active HIGH for 3/4bpp mode.
//
// SPIM plumbing is modeled on sharp_ls013b7dh01_nrf5.c.

#include "drivers/display/display.h"

#include "board/board.h"
#include "board/display.h"
#include "drivers/gpio.h"
#include "kernel/events.h"
#include "system/passert.h"
#include "util/reverse.h"

#include <hal/nrf_gpio.h>
#include <nrfx_spim.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// 3bpp update-mode command byte (before bit-reversal for LSB-first clocking).
#define LCD_CMD_UPDATE_3BPP 0x80U

// Panel geometry (physical 176x176, native 3bpp).
#define LCD_PANEL_WIDTH PANEL_DISPLAY_WIDTH
#define LCD_PANEL_HEIGHT PANEL_DISPLAY_HEIGHT
#define LCD_BITS_PER_PIXEL 3

// Bytes of pixel payload per panel line: 176 * 3 = 528 bits = 66 bytes.
#define LCD_LINE_DATA_BYTES ((LCD_PANEL_WIDTH * LCD_BITS_PER_PIXEL + 7) / 8)
// 2-byte header (command + line address) precedes each line's pixel payload.
#define LCD_ROW_HEADER_BYTES 2
#define LCD_LINE_STRIDE (LCD_ROW_HEADER_BYTES + LCD_LINE_DATA_BYTES)
// Full frame: every panel line plus 2 trailing dummy bytes to flush the panel.
#define LCD_FRAME_BYTES (LCD_LINE_STRIDE * LCD_PANEL_HEIGHT + 2)

// 3bpp color words.
#define LCD_COLOR_WHITE 0x7U
#define LCD_COLOR_BLACK 0x0U
// Letterbox border fill (the margins around the 144x168 UI).
#define LCD_BORDER_COLOR LCD_COLOR_BLACK

// Mono framebuffer geometry (source UI).
#define FB_ROW_BYTES (((PBL_DISPLAY_WIDTH) + 7) / 8)

// Persistent mono framebuffer. display_update() may hand us only the dirty
// rows, so we retain the full image here and re-encode the whole panel each
// update; the memory-in-pixel panel keeps whatever we do not rewrite anyway.
static uint8_t s_framebuffer[PBL_DISPLAY_HEIGHT * FB_ROW_BYTES];

// DMA transmit buffer for one full panel frame (lives in RAM for EasyDMA).
static uint8_t s_frame[LCD_FRAME_BYTES];

static bool s_updating;
static bool s_rotated_180;
static UpdateCompleteCallback s_uccb;
static SemaphoreHandle_t s_sem;

static inline void prv_enable_spim(void) { nrf_spim_enable(BOARD_CONFIG_DISPLAY.spi.p_reg); }

static inline void prv_disable_spim(void) {
  nrf_spim_disable(BOARD_CONFIG_DISPLAY.spi.p_reg);

  // Workaround for nRF52840 anomaly 195.
  if (BOARD_CONFIG_DISPLAY.spi.p_reg == NRF_SPIM3) {
    *(volatile uint32_t *)0x4002F004 = 1;
  }
}

// CS is active HIGH on this panel (3/4bpp mode).
static inline void prv_enable_chip_select(void) {
  gpio_output_set(&BOARD_CONFIG_DISPLAY.cs, true);
}

static inline void prv_disable_chip_select(void) {
  gpio_output_set(&BOARD_CONFIG_DISPLAY.cs, false);
}

// Set the 3bpp color of panel pixel x within a zeroed line payload, LSB-first.
static inline void prv_pack_pixel(uint8_t *data, int x, uint8_t color) {
  if (color == 0) {
    return;  // buffer is pre-zeroed; nothing to set for black.
  }
  const int bit = x * LCD_BITS_PER_PIXEL;
  const int byte = bit >> 3;
  const int off = bit & 7;
  const uint16_t v = (uint16_t)(color & 0x7U) << off;
  data[byte] |= (uint8_t)(v & 0xFFU);
  const uint8_t hi = (uint8_t)(v >> 8);
  if (hi != 0) {
    // The final pixel (x=175) ends exactly on the byte boundary, so a nonzero
    // high byte only ever lands inside the payload.
    PBL_ASSERTN(byte + 1 < LCD_LINE_DATA_BYTES);
    data[byte + 1] |= hi;
  }
}

// Read one mono framebuffer pixel; returns true for a set (white) pixel.
static inline bool prv_fb_pixel(int fb_x, int fb_y) {
  const uint8_t *row = &s_framebuffer[fb_y * FB_ROW_BYTES];
  return (row[fb_x >> 3] >> (fb_x & 7)) & 1U;
}

// Encode the retained mono framebuffer into the full-panel DMA buffer.
static void prv_encode_frame(void) {
  memset(s_frame, 0, sizeof(s_frame));

  uint8_t *line = s_frame;
  for (int y = 0; y < LCD_PANEL_HEIGHT; y++) {
    const int panel_y = s_rotated_180 ? (LCD_PANEL_HEIGHT - 1 - y) : y;
    line[0] = (uint8_t)reverse_byte(LCD_CMD_UPDATE_3BPP);
    line[1] = (uint8_t)reverse_byte((uint8_t)(y + 1));

    uint8_t *data = &line[LCD_ROW_HEADER_BYTES];
    const int fb_y = panel_y - LETTERBOX_OFFSET_Y;
    const bool row_in_fb = (fb_y >= 0) && (fb_y < PBL_DISPLAY_HEIGHT);

    for (int x = 0; x < LCD_PANEL_WIDTH; x++) {
      uint8_t color = LCD_BORDER_COLOR;
      if (row_in_fb) {
        const int fb_x = (s_rotated_180 ? (LCD_PANEL_WIDTH - 1 - x) : x) - LETTERBOX_OFFSET_X;
        if (fb_x >= 0 && fb_x < PBL_DISPLAY_WIDTH) {
          color = prv_fb_pixel(fb_x, fb_y) ? LCD_COLOR_WHITE : LCD_COLOR_BLACK;
        }
      }
      prv_pack_pixel(data, x, color);
    }
    line += LCD_LINE_STRIDE;
  }
  // Trailing dummy bytes already zeroed by memset.
}

static void prv_terminate_transfer(void *data) {
  s_updating = false;
  prv_disable_chip_select();
  prv_disable_spim();
  if (s_uccb) {
    s_uccb();
  }
}

static void prv_spim_evt_handler(nrfx_spim_evt_t const *evt, void *ctx) {
  portBASE_TYPE woken = pdFALSE;

  if (s_updating) {
    PebbleEvent e = {
        .type = PEBBLE_CALLBACK_EVENT,
        .callback = {.callback = prv_terminate_transfer},
    };
    woken = event_put_isr(&e) ? pdTRUE : pdFALSE;
  } else {
    xSemaphoreGiveFromISR(s_sem, &woken);
  }

  portEND_SWITCHING_ISR(woken);
}

void display_init(void) {
  nrfx_spim_config_t config = NRFX_SPIM_DEFAULT_CONFIG(
      BOARD_CONFIG_DISPLAY.clk.gpio_pin, BOARD_CONFIG_DISPLAY.mosi.gpio_pin,
      NRF_SPIM_PIN_NOT_CONNECTED, NRF_SPIM_PIN_NOT_CONNECTED);
  config.frequency = NRFX_MHZ_TO_HZ(4);
  config.bit_order = NRF_SPIM_BIT_ORDER_LSB_FIRST;

  nrfx_err_t err = nrfx_spim_init(&BOARD_CONFIG_DISPLAY.spi, &config, prv_spim_evt_handler, NULL);
  PBL_ASSERTN(err == NRFX_SUCCESS);

  gpio_output_init(&BOARD_CONFIG_DISPLAY.cs, GPIO_OType_PP);
  prv_disable_chip_select();

  // DISP/enable high at init keeps the panel showing memory contents.
  gpio_output_init(&BOARD_CONFIG_DISPLAY.on_ctrl,
                   (GPIOOType_TypeDef)BOARD_CONFIG_DISPLAY.on_ctrl_otype);
  gpio_output_set(&BOARD_CONFIG_DISPLAY.on_ctrl, true);

  s_sem = xSemaphoreCreateBinary();
}

void display_clear(void) {
  memset(s_framebuffer, 0, sizeof(s_framebuffer));
  prv_encode_frame();

  nrfx_spim_xfer_desc_t desc = {.p_tx_buffer = s_frame, .tx_length = sizeof(s_frame)};

  PBL_ASSERTN(!s_updating);
  prv_enable_spim();
  prv_enable_chip_select();

  nrfx_err_t err = nrfx_spim_xfer(&BOARD_CONFIG_DISPLAY.spi, &desc, 0);
  PBL_ASSERTN(err == NRFX_SUCCESS);
  xSemaphoreTake(s_sem, portMAX_DELAY);

  prv_disable_chip_select();
  prv_disable_spim();
}

void display_set_enabled(bool enabled) {
  gpio_output_set(&BOARD_CONFIG_DISPLAY.on_ctrl, enabled);
}

void display_set_rotated(bool rotated) { s_rotated_180 = rotated; }

void display_update(NextRowCallback nrcb, UpdateCompleteCallback uccb) {
  DisplayRow row;

  PBL_ASSERTN(!s_updating);

  // Absorb the offered rows into the retained mono framebuffer.
  while (nrcb(&row)) {
    if (row.address < PBL_DISPLAY_HEIGHT) {
      memcpy(&s_framebuffer[row.address * FB_ROW_BYTES], row.data, FB_ROW_BYTES);
    }
  }

  prv_encode_frame();

  nrfx_spim_xfer_desc_t desc = {.p_tx_buffer = s_frame, .tx_length = sizeof(s_frame)};

  prv_enable_spim();
  prv_enable_chip_select();

  s_uccb = uccb;
  s_updating = true;

  nrfx_err_t err = nrfx_spim_xfer(&BOARD_CONFIG_DISPLAY.spi, &desc, 0);
  PBL_ASSERTN(err == NRFX_SUCCESS);
}

bool display_update_in_progress(void) { return s_updating; }

// Boot-animation frame push, only used on the CONFIG_PBLBOOT path (not enabled
// on bangle2). The previous implementation copied the caller's 8bpp boot
// framebuffer directly into the retained 1bpp mono buffer, which mismatches the
// display.h contract (stride/bit-depth) and would render garbage. A
// wrong-looking-right implementation is worse than none, so leave it an empty
// stub (as sharp_ls013b7dh01_nrf5.c does) until a boot path actually needs it.
void display_update_boot_frame(uint8_t *framebuffer) {}
