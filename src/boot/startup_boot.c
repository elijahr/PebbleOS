/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Bootloader reset handler. Initializes .data/.bss, runs SoC system init and
//! the bangle2 REGOUT0 guard, then hands off to boot_main().
//!
//! Modeled on src/fw/startup/startup_cortex_m.c but standalone: no cache
//! enable, no pbl attribute macros. SystemInit comes from the nrfx MDK
//! (system_nrf52840.c) so FPU/errata init matches the firmware.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_BOARD_BANGLE2)
// nRF register definitions (NRF_UICR, NRF_NVMC, UICR_REGOUT0_*, NVMC_*) and
// CMSIS NVIC_SystemReset() for the REGOUT0 guard below.
#include <nrfx.h>
#endif

//! Linker-defined symbols for section initialization. Arrays avoid needing an
//! & and let us do pointer arithmetic on the lengths.
extern uint8_t __data_load_start[];
extern uint8_t __data_start[];
extern uint8_t __data_end[];
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];
extern uint8_t _estack[];

//! Bootloader entry point (real logic lands next).
extern void boot_main(void);

//! SoC-specific system initialization (nrfx system_nrf52840.c).
extern void SystemInit(void);

void Reset_Handler(void) {
  // Copy the .data section from its flash load address into RAM.
  memcpy(__data_start, __data_load_start, __data_end - __data_start);

  // Zero the .bss section (assumes .bss follows .data).
  memset(__bss_start, 0, __bss_end - __bss_start);

  SystemInit();

#if defined(CONFIG_BOARD_BANGLE2)
  // Bangle.js 2: ensure GPIO supply is 3.3 V (not the 1.8 V default) after a
  // UICR-clearing recovery (mass_erase). Copied verbatim from
  // startup_cortex_m.c. Write ONLY when the field is at reset default (pure
  // 1->0 NVMC program; no erase). NEVER page-erase UICR: that clears APPROTECT
  // and can brick the debug port on new silicon.
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

  boot_main();

  // boot_main shouldn't return.
  while (true) {
  }
}
