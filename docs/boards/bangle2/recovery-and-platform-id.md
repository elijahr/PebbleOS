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

Registration is a TRADE, not a pure win. It has three consequences, two
good and one that points at the hardware. (An earlier draft of this doc
called registration "strictly beneficial" — that was wrong, and is
corrected here; credit pebble-noti-2 for catching it before it hardened
into inherited testimony.)

1. GOOD: it fixes the wrong-watchType symptom. Today platform 22 -> UNKNOWN
   -> the app's `unknownWatchTypePlatform` fallback (EMERY, a COLOUR
   platform), so it offers colour app variants to a 1-bit mono watch.
   Registered, it resolves to FLINT (correct).
2. GOOD: it adds a sideload guard. After registration only `hwrev =
   "bangle2"` images sideload; a mismatched-platform image is rejected.
3. **RISK: it REMOVES a firmware-update-check interlock.** This is the one
   nobody accounted for at first. `FirmwareUpdateCheck.kt:66-70`:

       watch.platform == UNKNOWN -> UpdateCheckFailed("Unknown platform")
       watch.platform.isCoreDevice() && MEMFAULT_TOKEN != null -> memfault…
       else -> cohorts.getLatestFirmware(watch)

   Verified in source: today `platform == UNKNOWN` SHORT-CIRCUITS before any
   update check runs. The "Unknown platform" line we have been treating as a
   cosmetic annoyance is in fact acting as a SAFETY INTERLOCK — it is why
   this watch has never been offered a firmware update. `isCoreDevice()`
   (:77-85) is a false-list ending in `else -> true`, and BANGLE2 is not in
   the false-list, so it returns `true`. So after registration the check
   proceeds to a live remote service (`memfault` or `cohorts`) for a watch
   running a hand-built PebbleOS image with a hand-installed PRF, on the one
   piece of hardware in this project that is not recoverable without SWD and
   whose external NOR cannot yet be restored from a probe.

   NOT verified, stated plainly: whether `MEMFAULT_TOKEN` is set in the
   store build, and what either service returns for platform 22 (plausibly
   nothing, since no such device exists upstream). The claim is not "an
   update will be offered" — it is "the interlock is removed, and the
   outcome then depends on a remote service nobody here controls or has
   tested." "It probably returns nothing" is not a safety argument for a
   brickable single-instance device.

So the honest summary: registration ADDS a sideload safety check while
REMOVING an update-check interlock. The same UNKNOWN state that causes the
wrong-watchType bug also provides the interlock; fixing the bug removes the
interlock. Trade, not pure win.

### DECISION (2026-08-01): hold RELEASED, but registration is GUARDED

Two separate decisions:

- The original hold's PREMISE (registration closes the recovery sideload
  channel) is disproven by the enum analysis above. That hold is released.
- BUT registration is gated on a mitigation for consequence 3.
  **OPERATOR DECISION (via pebble-noti-2, 2026-08-01): enable the phone
  app's "Disable FW update notifications" Debug setting FIRST, confirm it
  holds, THEN push the registration.** That setting exists precisely for
  users who sideload their own firmware. The registration push is gated on
  the guard, not on further debate; it was blocked only by the phone being
  physically disconnected at decision time.

The decision to land the registration PR is the operator's; this doc
records the analysis, the trade, and the agreed sequencing.

### Open question (deferred): should BANGLE2 be a "Core device" for updates?

`isCoreDevice()` returns `true` for BANGLE2 only by falling through
`else -> true`, not by a deliberate choice. Whether a bring-up board should
be treated as a Core device for firmware-update purposes (which decides
whether Memfault or the cohorts service is queried) is a real question that
should be decided consciously, not inherited by omission. It is out of
scope for the minimal platform-id registration and is left for a separate,
explicitly-reasoned change if the answer is "no". The operator's
"Disable FW update notifications" mitigation covers the immediate risk
regardless of how this is answered.

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
