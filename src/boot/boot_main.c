/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! bangle2 bootloader: selects the recovery firmware (PRF) or the normal
//! firmware at reset and starts it.
//!
//! The firmware is linked at internal flash 0x8000 (behind this 32 KiB
//! bootloader). External SPI-NOR holds the authoritative images: SAFE_FIRMWARE
//! (0x000000) is the PRF; FIRMWARE_SLOT_1 (0x100000) is the OTA staging slot.
//! When a boot bit asks for it, the bootloader copies an image out of external
//! NOR into the internal execution slot and starts it there; otherwise it
//! starts whatever already lives at 0x8000.
//!
//! Design: plans/2026-07-28-bangle2-r10-bootloader-design.md (sections 9, 2.4).
//! Watchdog ownership is intentionally NOT taken here yet: the firmware still
//! self-arms (CONFIG_WATCHDOG_SELF_ARM), and no WDT is armed while this code
//! runs, so the copy path has no watchdog to feed. That is a later increment.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nrfx.h>

#include "boot_image.h"
#include "boot_select.h"
#include "pbl/util/crc32.h"

// -----------------------------------------------------------------------------
// Layout constants

//! Internal flash execution slot: the firmware (normal or PRF) runs here.
#define FW_EXEC_BASE 0x00008000u
//! Internal flash page size (nRF52840).
#define NVMC_PAGE_SIZE 0x1000u
//! Top of the internal flash slot the bootloader is allowed to write.
//! FLASH_BASE(0) + FW_FLASH_SIZE(0xD1000). The firmware image must fit below.
#define FW_EXEC_END 0x000D9000u

// Boot-bit and NOR-source constants + the boot-selection state machine live in
// boot_select.h (unit-tested off-target).

// -----------------------------------------------------------------------------
// Retained page (mirror of src/fw/system + rtc_registers.h)

#define RETAINED_BASE 0x20000000u
#define RETAINED_WORDS 64u       // 256 bytes
#define RETAINED_CRC_IDX 31u     // NRF_RETAINED_REGISTER_CRC
#define RETAINED_BOOTBIT_IDX 0u  // RTC_BKP_BOOTBIT_DR

//! Legacy firmware image header at the start of an image (firmware_storage.h).
typedef struct __attribute__((packed)) {
  uint32_t description_length;  // must be 12
  uint32_t firmware_length;
  uint32_t checksum;  // crc32 over the image bytes after this header
} FirmwareDescription;

// FW_DESCRIPTION_LENGTH and the pure geometry/CRC validation decisions live in
// boot_image.h (unit-tested off-target).

// -----------------------------------------------------------------------------
// Retained page helpers. The page holds the CRC of its own first 31 words in
// word 31; a page that fails the CRC is noise from a cold power-on and its
// boot bits must not be trusted.

static uint32_t s_retained[RETAINED_WORDS];
static bool s_retained_valid;

static void retained_load(void) {
  const volatile uint32_t *src = (const volatile uint32_t *)RETAINED_BASE;
  for (uint32_t i = 0; i < RETAINED_WORDS; i++) {
    s_retained[i] = src[i];
  }
  uint32_t computed = crc32(0, s_retained, RETAINED_CRC_IDX * sizeof(uint32_t));
  s_retained_valid = (computed == s_retained[RETAINED_CRC_IDX]);
}

//! Mutate a boot bit and write the page (with a freshly recomputed CRC) back to
//! the retained region. Matches the firmware's retained_write() CRC contract so
//! the firmware's boot_bit_init() accepts the page instead of wiping it.
static void boot_bits_update(uint32_t set_mask, uint32_t clear_mask) {
  volatile uint32_t *dst = (volatile uint32_t *)RETAINED_BASE;
  s_retained[RETAINED_BOOTBIT_IDX] = (s_retained[RETAINED_BOOTBIT_IDX] & ~clear_mask) | set_mask;
  s_retained[RETAINED_CRC_IDX] = crc32(0, s_retained, RETAINED_CRC_IDX * sizeof(uint32_t));
  for (uint32_t i = 0; i < RETAINED_WORDS; i++) {
    dst[i] = s_retained[i];
  }
  s_retained_valid = true;
}

// -----------------------------------------------------------------------------
// External SPI-NOR read over SPIM2 (blocking, single data line, opcode 0x03).
// Pins: CS P0.14 (active low, GPIO), SCK P0.16, MOSI P0.15, MISO P0.13.
// Registers verified against the MDK header; identical to the sequence proven
// host-side over SWD (design section 12.1).

#define NOR_CS_PIN 14u
#define NOR_SCK_PIN 16u
#define NOR_MOSI_PIN 15u
#define NOR_MISO_PIN 13u
#define NOR_OP_READ 0x03u

//! One SPIM2 command's worth of data. 4-byte header (opcode + 24-bit address)
//! then up to a page of streamed read data. In .bss (RAM) for EasyDMA.
#define NOR_CHUNK 256u
static uint8_t s_nor_tx[4];
static uint8_t s_nor_rx[4 + NOR_CHUNK] __attribute__((aligned(4)));

static void nor_init(void) {
  // CS as a GPIO output, idle high (deasserted).
  NRF_P0->PIN_CNF[NOR_CS_PIN] = (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
                                (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
  NRF_P0->OUTSET = (1u << NOR_CS_PIN);

  NRF_SPIM2->PSEL.SCK = NOR_SCK_PIN;
  NRF_SPIM2->PSEL.MOSI = NOR_MOSI_PIN;
  NRF_SPIM2->PSEL.MISO = NOR_MISO_PIN;
  NRF_SPIM2->CONFIG = 0;  // SPI mode 0, MSB first
  NRF_SPIM2->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M8;
  NRF_SPIM2->ORC = 0xFF;
  NRF_SPIM2->ENABLE = (SPIM_ENABLE_ENABLE_Enabled << SPIM_ENABLE_ENABLE_Pos);
}

//! Read `len` (<= NOR_CHUNK) bytes from external NOR `addr` into `dst`.
static void nor_read(uint32_t addr, uint8_t *dst, uint32_t len) {
  s_nor_tx[0] = NOR_OP_READ;
  s_nor_tx[1] = (uint8_t)(addr >> 16);
  s_nor_tx[2] = (uint8_t)(addr >> 8);
  s_nor_tx[3] = (uint8_t)(addr);

  NRF_SPIM2->TXD.PTR = (uint32_t)s_nor_tx;
  NRF_SPIM2->TXD.MAXCNT = sizeof(s_nor_tx);
  NRF_SPIM2->RXD.PTR = (uint32_t)s_nor_rx;
  NRF_SPIM2->RXD.MAXCNT = 4 + len;  // 4 bytes clocked in during opcode+address
  NRF_SPIM2->EVENTS_END = 0;

  NRF_P0->OUTCLR = (1u << NOR_CS_PIN);  // assert CS
  NRF_SPIM2->TASKS_START = 1;
  while (NRF_SPIM2->EVENTS_END == 0) {
  }
  NRF_P0->OUTSET = (1u << NOR_CS_PIN);  // deassert CS

  memcpy(dst, &s_nor_rx[4], len);  // skip the 4 header bytes
}

//! Return SPIM2 and the NOR chip-select pin to their reset state. The firmware
//! must inherit hardware exactly as it would after a cold reset (see the
//! reset-equivalent handoff contract in start_firmware): an SPIM2 left enabled
//! with its PSEL routed, or a GPIO left driven, is state the firmware was never
//! written to tolerate and would surface as a fault far from this cause. NVMC
//! is already restored to WEN_Ren by each erase/write; PSEL is only writable
//! while the peripheral is disabled, so disable first, then disconnect.
static void nor_deinit(void) {
  NRF_SPIM2->ENABLE = (SPIM_ENABLE_ENABLE_Disabled << SPIM_ENABLE_ENABLE_Pos);
  NRF_SPIM2->PSEL.SCK = 0xFFFFFFFFu;   // CONNECT=Disconnected (PSEL reset value)
  NRF_SPIM2->PSEL.MOSI = 0xFFFFFFFFu;
  NRF_SPIM2->PSEL.MISO = 0xFFFFFFFFu;
  NRF_P0->OUTCLR = (1u << NOR_CS_PIN);
  NRF_P0->PIN_CNF[NOR_CS_PIN] = 0x00000002u;  // GPIO PIN_CNF reset value
}

// -----------------------------------------------------------------------------
// Internal flash erase/write (NVMC). Same idiom as the startup REGOUT0 guard.

static void nvmc_wait(void) {
  while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
  }
}

static void nvmc_erase_range(uint32_t start, uint32_t end) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
  nvmc_wait();
  for (uint32_t page = start & ~(NVMC_PAGE_SIZE - 1); page < end; page += NVMC_PAGE_SIZE) {
    NRF_NVMC->ERASEPAGE = page;
    nvmc_wait();
  }
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
  nvmc_wait();
}

//! Write `len` bytes (word-aligned length) from `src` to internal flash `dst`.
static void nvmc_write(uint32_t dst, const uint8_t *src, uint32_t len) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
  nvmc_wait();
  for (uint32_t off = 0; off < len; off += 4) {
    uint32_t word;
    memcpy(&word, &src[off], 4);
    *(volatile uint32_t *)(dst + off) = word;
    nvmc_wait();
  }
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
  nvmc_wait();
}

// -----------------------------------------------------------------------------
// Image validation + copy

//! Validate the legacy FirmwareDescription at external NOR `src_addr`. The
//! stored image is [12-byte FirmwareDescription][payload]; the payload is the
//! executable firmware, whose vector table is at its first byte (the firmware
//! links with its vector table AT FW_EXEC_BASE -- CONFIG_FIRMWARE_OFFSET is 0
//! for this board, so the execution image reserves NO room for the header).
//! On success writes the payload length and its expected CRC to the out params.
static bool image_validate(uint32_t src_addr, uint32_t *out_payload_len, uint32_t *out_checksum) {
  FirmwareDescription desc;
  nor_read(src_addr, (uint8_t *)&desc, sizeof(desc));
  if (!boot_image_geometry_valid(desc.description_length, desc.firmware_length,
                                 FW_EXEC_END - FW_EXEC_BASE)) {
    return false;
  }
  // CRC over the payload that follows the 12-byte header.
  uint32_t crc = 0;
  uint32_t remaining = desc.firmware_length;
  uint32_t addr = src_addr + FW_DESCRIPTION_LENGTH;
  static uint8_t buf[NOR_CHUNK];
  while (remaining > 0) {
    uint32_t chunk = remaining < NOR_CHUNK ? remaining : NOR_CHUNK;
    nor_read(addr, buf, chunk);
    crc = crc32(crc, buf, chunk);
    addr += chunk;
    remaining -= chunk;
  }
  if (!boot_image_checksum_valid(crc, desc.checksum)) {
    return false;
  }
  *out_payload_len = desc.firmware_length;
  *out_checksum = desc.checksum;
  return true;
}

//! Copy the `payload_len`-byte firmware payload from external NOR -- skipping
//! the 12-byte FirmwareDescription header at `src_addr` -- into the internal
//! execution slot at FW_EXEC_BASE, erasing first. The header is metadata and is
//! NOT copied: FW_EXEC_BASE must hold the firmware's vector table so the CPU can
//! boot it. Assumes image_validate() passed.
static void image_copy(uint32_t src_addr, uint32_t payload_len) {
  const uint32_t payload_src = src_addr + FW_DESCRIPTION_LENGTH;
  nvmc_erase_range(FW_EXEC_BASE, FW_EXEC_BASE + payload_len);
  static uint8_t buf[NOR_CHUNK];
  uint32_t off = 0;
  while (off < payload_len) {
    uint32_t chunk = (payload_len - off) < NOR_CHUNK ? (payload_len - off) : NOR_CHUNK;
    // NVMC writes whole words; round the tail up (NOR read gives us the bytes,
    // the erased flash beyond the image reads back as 0xFF regardless).
    uint32_t wchunk = (chunk + 3u) & ~3u;
    nor_read(payload_src + off, buf, chunk);
    if (wchunk > chunk) {
      memset(&buf[chunk], 0xFF, wchunk - chunk);
    }
    nvmc_write(FW_EXEC_BASE + off, buf, wchunk);
    off += chunk;
  }
}

//! Re-read the just-written internal payload and confirm its CRC against the
//! value from the source header. The header is not present at FW_EXEC_BASE (it
//! was stripped on copy), so validate the payload bytes directly.
static bool internal_image_valid(uint32_t payload_len, uint32_t checksum) {
  uint32_t crc = crc32(0, (const uint8_t *)FW_EXEC_BASE, payload_len);
  return boot_image_checksum_valid(crc, checksum);
}

// -----------------------------------------------------------------------------
// Start the image at FW_EXEC_BASE.

//! The image at FW_EXEC_BASE starts with its vector table: word 0 is the
//! initial SP, word 1 is the reset vector. Point VTOR at it, load SP, jump.
//!
//! Reset-equivalent handoff contract: the firmware is written to start from a
//! cold reset, so every machine-state input it does not itself initialize must
//! match the reset default at the moment of the jump. This function restores
//! PRIMASK (below); any peripheral the copy path touches is torn down before we
//! get here (nor_deinit for SPIM2/GPIO, NVMC self-restores to WEN_Ren). The
//! bootloader deliberately arms no WDT and never leaves MSP/CONTROL/FAULTMASK
//! off their reset values. Anything added here that enables a peripheral, a
//! timer, an IRQ, or a clock source must be undone before this point.
__attribute__((noreturn)) static void start_firmware(void) {
  const uint32_t *vt = (const uint32_t *)FW_EXEC_BASE;
  uint32_t sp = vt[0];
  uint32_t pc = vt[1];

  __disable_irq();
  SCB->VTOR = FW_EXEC_BASE;
  __DSB();
  __ISB();
  __asm volatile("msr msp, %0\n" : : "r"(sp) : "memory");
  // Restore the post-reset interrupt state the firmware expects. A firmware
  // that boots at 0x0 straight from a hardware reset runs with PRIMASK clear.
  // We set PRIMASK above only to keep the VTOR/MSP switch atomic; clear it now
  // (cpsie i -- a single instruction, no stack use, safe after the msp swap)
  // so the firmware starts exactly as it would after reset. Leaving PRIMASK
  // set makes FreeRTOS's `svc 0` at xPortStartScheduler unable to be taken,
  // escalating to a forced HardFault (HFSR.FORCED, CFSR==0).
  __enable_irq();
  __asm volatile("bx %0\n" : : "r"(pc));
  __builtin_unreachable();
}

// -----------------------------------------------------------------------------

void boot_main(void) {
  retained_load();

  const uint32_t boot_bits = s_retained_valid ? s_retained[RETAINED_BOOTBIT_IDX] : 0;
  const BootPlan plan = boot_select(boot_bits, s_retained_valid);

  if (plan.want_copy) {
    nor_init();
    uint32_t payload_len = 0;
    uint32_t checksum = 0;
    if (image_validate(plan.src_addr, &payload_len, &checksum)) {
      // Validate-before-erase held: only now do we touch the execution slot.
      boot_bits_update(BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS, 0);
      image_copy(plan.src_addr, payload_len);
      if (internal_image_valid(payload_len, checksum)) {
        boot_bits_update(plan.install_bit, plan.consume_bit | BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);
      } else {
        // Copy landed a bad image. Record a strike, clear the consume bit, and
        // fall through to start whatever is in the slot (may still be the old,
        // good firmware if the erase/copy was interrupted — the strike lets a
        // later pass give up).
        boot_bits_update(boot_next_strike(&plan, boot_bits),
                         plan.consume_bit | BOOT_BIT_NEW_FW_UPDATE_IN_PROGRESS);
      }
    } else {
      // Invalid source image. The slot was NEVER erased: the resident image is
      // intact. Record a strike, clear the request, start the resident image.
      boot_bits_update(boot_next_strike(&plan, boot_bits), plan.consume_bit);
    }
    // image_validate() already drove SPIM2 on every sub-path above, so tear it
    // down here (not per-branch) to honor the reset-equivalent handoff.
    nor_deinit();
  }

  start_firmware();
}
