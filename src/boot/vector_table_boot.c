/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Minimal Cortex-M4 vector table for the bootloader.
//!
//! Only the 16 core exception vectors are populated. The bootloader polls
//! rather than servicing external nRF52 IRQs, so no external interrupt
//! entries are needed. Self-contained (plain weak aliases, no pbl macros).

#include <stdbool.h>
#include <stdint.h>

extern uint8_t _estack[];
extern void Reset_Handler(void);

void Default_Handler(void) {
  // Unexpected exception: spin so a debugger can inspect the state.
  while (true) {
  }
}

// Weak aliases to Default_Handler; a real handler defined elsewhere overrides.
__attribute__((weak, alias("Default_Handler"))) void NMI_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void HardFault_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void MemManage_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void BusFault_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void UsageFault_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void SVC_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void DebugMon_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void PendSV_Handler(void);
__attribute__((weak, alias("Default_Handler"))) void SysTick_Handler(void);

__attribute__((used, section(".isr_vector"))) const void *const vector_table[] = {
    _estack,             // 0: initial stack pointer
    Reset_Handler,       // 1: reset
    NMI_Handler,         // 2
    HardFault_Handler,   // 3
    MemManage_Handler,   // 4
    BusFault_Handler,    // 5
    UsageFault_Handler,  // 6
    0,                   // 7  (reserved)
    0,                   // 8  (reserved)
    0,                   // 9  (reserved)
    0,                   // 10 (reserved)
    SVC_Handler,         // 11
    DebugMon_Handler,    // 12
    0,                   // 13 (reserved)
    PendSV_Handler,      // 14
    SysTick_Handler,     // 15
};
