/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Initial firmware startup, contains the vector table that the bootloader loads.
//! Based on "https://github.com/pfalcon/cortex-uni-startup/blob/master/startup.c"
//! by Paul Sokolovsky (public domain)

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "pbl/mcu/cache.h"
#include "pbl/util/attributes.h"

#if defined(CONFIG_BOARD_BANGLE2)
// nRF register definitions (NRF_UICR, NRF_NVMC, UICR_REGOUT0_*, NVMC_*) and
// CMSIS NVIC_SystemReset() for the REGOUT0 guard below. Gated: this file is
// shared with non-nRF Cortex-M boards.
#include <nrfx.h>
#endif

//! These symbols are defined in the linker script for use in initializing
//! the data sections. uint8_t since we do arithmetic with section lengths.
//! These are arrays to avoid the need for an & when dealing with linker symbols.
extern uint8_t __data_load_start[];
extern uint8_t __data_start[];
extern uint8_t __data_end[];
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];
extern uint8_t _estack[];

//! Firmware main function, ResetHandler calls this
extern int main(void);

//! SoC-specific system initialization (e.g. nRF52 system_nrf52840.c)
extern void SystemInit(void);

//! This function is what gets called when the processor first
//! starts execution following a reset event. The data and bss
//! sections are initialized, then we call the firmware's main
//! function
NORETURN Reset_Handler(void) {
  // Copy data section from flash to RAM
  memcpy(__data_start, __data_load_start, __data_end - __data_start);

  // Clear the bss section, assumes .bss goes directly after .data
  memset(__bss_start, 0, __bss_end - __bss_start);

  SystemInit();

#if defined(CONFIG_BOARD_BANGLE2)
  // Bangle.js 2: ensure GPIO supply is 3.3 V (not the 1.8 V default) after a
  // UICR-clearing recovery (mass_erase). Copied from Espruino
  // nrf5x_utils.c nrf_configure_uicr_flags(); same idiom as the vendor
  // NFCPINS block in SystemInit. Write ONLY when the field is at reset
  // default (pure 1->0 NVMC program; no erase). NEVER page-erase UICR: that
  // clears APPROTECT and can brick the debug port on new silicon.
  if ((NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) ==
      (UICR_REGOUT0_VOUT_DEFAULT << UICR_REGOUT0_VOUT_Pos)) {
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    }
    NRF_UICR->REGOUT0 = (NRF_UICR->REGOUT0 & ~((uint32_t)UICR_REGOUT0_VOUT_Msk)) |
                        (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    }
    NVIC_SystemReset();  // UICR changes apply at reset.
  }
#endif

  icache_enable();
  dcache_enable();

  main();

  // Main shouldn't return
  while (true) {}
}
