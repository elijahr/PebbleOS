<!--
SPDX-FileCopyrightText: 2026 Core Devices LLC
SPDX-License-Identifier: Apache-2.0
-->

# Bangle.js 2: recovery channel and the BANGLE2(22) platform-ID decision

Owner: the R10 bootloader effort. This records a decision that was living
only in cross-session chat, where several other "true-when-said, stale
later" claims on this port already cost time. Committed so it is durable.

## The recovery / firmware-delivery channel

The watch has no bootloader yet, but the shipped firmware already contains
a complete over-BLE firmware-receive path, and it is the only host-side
way to write the external SPI-NOR today (SWD cannot reach that part; it is
not memory-mapped — see `index.md`).

Chain, firmware side:

- Phone sends a firmware image with BLE PutBytes. A recovery image goes as
  `ObjectRecovery` and lands in the scratch slot `FLASH_REGION_FIRMWARE_SLOT_1`
  (external NOR `0x100000`), then INSTALL sets `BOOT_BIT_NEW_PRF_AVAILABLE`
  (`src/fw/services/put_bytes/put_bytes.c`).
- On the next boot, `check_prf_update()` (`src/fw/main.c` ->
  `src/fw/services/prf_update/service.c`) validates the image and copies it
  into `FLASH_REGION_SAFE_FIRMWARE` (external NOR `0x000000`).
- A normal-firmware image sets `BOOT_BIT_NEW_FW_AVAILABLE` instead. On
  bangle2 that bit currently has NO consumer — copying a staged normal
  firmware into the execution slot is the bootloader's job (R10). See the
  R10 design doc.

This is the channel every recovery/OTA decision below depends on.

## The app-side platform gate, and why registering BANGLE2(22) is SAFE

The phone app (coredevices `mobileapp`, `libpebble3`) refuses a firmware
whose board does not match the watch. The check is
`FirmwareUpdater.kt:125` (`libpebble3/.../connection/endpointmanager/`):

    watchPlatform != firmware.hwRev  ->  reject

Both operands are `WatchHardwarePlatform` enum values:

- `watchPlatform` = `WatchHardwarePlatform.fromProtocolNumber(<watch platform number>)`
  (`SystemService.kt:369`, fed to `FirmwareUpdater.init` at
  `TransportConnector.kt:222`). The bangle2 firmware reports platform
  number **22**.
- `firmware.hwRev` = the `.pbz` manifest `hwrev` STRING, deserialized via
  `WatchHardwarePlatformSerializer` -> `fromHWRevision(<string>)`. Our build
  system stamps `hwrev = "bangle2"` (verified against the deployed
  `recovery_bangle2_v4.30.0-78-gc6e8cde8.pbz`, sha256
  `6e19d69c…a95377`: `manifest.json` firmware.hwrev = `"bangle2"`).

The registration entry (mobileapp branch `bangle2-platform-id`) is
`BANGLE2(22u, WatchType.FLINT, "bangle2")` — it supplies BOTH
`protocolNumber = 22` AND `revision = "bangle2"`. Therefore:

| State | `fromProtocolNumber(22)` | `fromHWRevision("bangle2")` | check |
|-------|--------------------------|----------------------------|-------|
| Before registration | UNKNOWN | UNKNOWN | UNKNOWN == UNKNOWN -> PASS |
| After registration  | BANGLE2 | BANGLE2 | BANGLE2 == BANGLE2 -> PASS |

Both operands move `UNKNOWN -> BANGLE2` **together**. The equality holds in
both worlds, so **registering BANGLE2(22) does NOT close the recovery
sideload channel** for our `hwrev = "bangle2"` images.

Registration is in fact strictly beneficial:

1. It fixes the wrong-watchType symptom: today platform 22 -> UNKNOWN ->
   the app's `unknownWatchTypePlatform` fallback (EMERY, a COLOUR platform),
   so it offers colour app variants to a 1-bit mono watch. Registered, it
   resolves to FLINT (correct).
2. It ADDS a correct guard: after registration only `hwrev = "bangle2"`
   images sideload; a mismatched-platform image is now rejected, which is
   what we want.

### DECISION (2026-08-01): the BANGLE2(22) hold is RELEASED

An earlier hold said "do not register / do not install an app with
BANGLE2(22) until a correctly-linked PRF or the SWD flashloader write path
exists," on the premise that registration would close the only recovery
channel. **That premise is disproven by the source analysis above.** The
hold is released. The decision to land the registration PR is the
operator's; this doc only removes the technical blocker and records why.

The pessimistic framing ("registering closes the channel") and the
"wrong watchType is live" symptom are the SAME lever, not opposed items:
the one change fixes the watchType and keeps recovery working.

## What is still a real constraint (firmware side, separate)

Independent of the app registration: the PRF currently in `SAFE_FIRMWARE`
is linked for flash base `0x0`. When the R10 bootloader moves the firmware
link base to `0x8000`, that resident PRF becomes unbootable and must be
re-delivered as a `0x8000` build over this same channel (which, per above,
survives registration). A CRC-valid-but-wrong-link image is NOT detectable
after the fact, so the link-base change series must invalidate the stale
`0x0` PRF. Full detail: the R10 bootloader design doc, sections 2.4 and 4.0.
This is internal to R10 sequencing and does not gate the registration.

## References

- App source cited above lives in the coredevices `mobileapp` repo
  (`libpebble3/`), branch `bangle2-platform-id`, not in this repo.
- R10 bootloader design doc (out-of-repo working doc):
  `2026-07-28-bangle2-r10-bootloader-design.md`.
