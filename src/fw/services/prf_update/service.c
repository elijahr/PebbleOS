/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "drivers/flash.h"
#include "flash_region/flash_region.h"
#include "system/bootbits.h"
#include "system/firmware_storage.h"
#include "system/logging.h"
#include "pbl/util/math.h"

PBL_LOG_MODULE_DEFINE(service_prf_update, CONFIG_SERVICE_PRF_UPDATE_LOG_LEVEL);

// Don't allow PRF updating when we're in PRF
#ifndef CONFIG_RECOVERY_FW
static void prv_do_update(void) {
  PBL_LOG_INFO("Updating PRF!");
  flash_prf_set_protection(false);

  bool saved_sleep_when_idle = flash_get_sleep_when_idle();
  flash_sleep_when_idle(false);

#ifndef CONFIG_PBLBOOT
  FirmwareDescription description =
      firmware_storage_read_firmware_description(FLASH_REGION_FIRMWARE_DEST_BEGIN);

  if (!firmware_storage_check_valid_firmware_description(FLASH_REGION_FIRMWARE_DEST_BEGIN,
                                                         &description)) {
    PBL_LOG_WRN("Invalid recovery firmware CRC in SPI flash!");
    goto done;
  }

  const uint32_t total_length = description.description_length + description.firmware_length;
#else
  FirmwareHeader header =
      firmware_storage_read_firmware_header(FLASH_REGION_FIRMWARE_DEST_BEGIN);
  if (!firmware_storage_check_valid_firmware_header(FLASH_REGION_FIRMWARE_DEST_BEGIN,
                                                    &header)) {
    PBL_LOG_WRN("Invalid recovery firmware CRC in SPI flash!");
    goto done;
  }

  const uint32_t total_length = header.fw_start + header.fw_length;
#endif

  PBL_LOG_DBG("Erasing previous PRF...");
  flash_region_erase_optimal_range(FLASH_REGION_SAFE_FIRMWARE_BEGIN,
                                   FLASH_REGION_SAFE_FIRMWARE_BEGIN,
                                   FLASH_REGION_SAFE_FIRMWARE_BEGIN + total_length,
                                   FLASH_REGION_SAFE_FIRMWARE_END);

  PBL_LOG_DBG("Copying PRF from scratch to the PRF slot");
  uint8_t buffer[512];
  uint32_t offset = 0;
  while (offset < total_length) {
    const uint32_t chunk_size = MIN(sizeof(buffer), (total_length - offset));

    flash_read_bytes(buffer, FLASH_REGION_FIRMWARE_DEST_BEGIN + offset, chunk_size);
    flash_write_bytes(buffer, FLASH_REGION_SAFE_FIRMWARE_BEGIN + offset, chunk_size);

    offset += chunk_size;
  }

done:
  flash_prf_set_protection(true);
  flash_sleep_when_idle(saved_sleep_when_idle);
  PBL_LOG_DBG("Done!");
}
#endif

void check_prf_update(void) {
  if (!boot_bit_test(BOOT_BIT_NEW_PRF_AVAILABLE)) {
    return;
  }

#ifndef CONFIG_RECOVERY_FW
  // Clear the bit only AFTER the copy completes. prv_do_update() erases
  // SAFE_FIRMWARE and writes ~452 KiB to external flash; a power loss inside
  // that window used to leave SAFE_FIRMWARE part-written with the retry bit
  // already gone, so nothing re-read the still-good staged image in
  // FIRMWARE_SLOT_1 and recovery stayed broken until someone sent a new PRF
  // over BLE. Clearing afterwards makes an interrupted install retry on the
  // next boot. The operation is idempotent: it re-erases and re-copies from
  // the same source.
  prv_do_update();
#endif

  boot_bit_clear(BOOT_BIT_NEW_PRF_AVAILABLE);
}
