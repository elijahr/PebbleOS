# Bangle.js 2

This page is the SWD flash and restore runbook for the bare-metal
full-flash PebbleOS port. The firmware links at flash base `0x0` and owns
the vector table. There is no MBR, no SoftDevice, and no bootloader on the
watch. SWD is the only delivery path.

## History: why not BLE DFU

The stock Bangle.js 2 bootloader verifies DFU packages against Espruino's
signing key. We confirmed this on real hardware. BLE Secure DFU of custom
firmware is not possible. We removed the BLE-DFU tooling and the old
BLE-DFU procedure. Git history preserves the investigation.

## Artifacts

Run `./pbl configure --board bangle2 && ./pbl build`. The build produces
both artifacts:

| Artifact | Path | Target |
|---|---|---|
| Internal firmware | `build/pebbleos.hex` | Internal flash `0x0`, over SWD |
| System resources | `build/system_resources.pbpack` | External flash `0x200000` (= Espruino `0x60200000`) |

```{important}
The pbpack MUST come from the same build as the hex. The resource store
validates version and CRC against the firmware. A mismatch shows as
missing resources (sad watch) at boot.
```

## Prerequisites

- OpenOCD 0.12.0 or later, with the `nrf5` flash driver and
  `target/nrf52.cfg`.
- Probe: an ST-Link V2 (a cheap clone is sufficient).
  - Use `interface/stlink-dap.cfg` (DAP-direct) when the probe firmware is
    V2J24 or later. Update clone firmware with ST's STSW-LINK007 tool if
    necessary.
  - `interface/stlink.cfg` (HLA) also works, for flash, dump, and debug.
    But HLA cannot access the CTRL-AP, and CTRL-AP ERASEALL
    (`nrf52_recover`) is the ONLY recovery from an APPROTECT lock. An
    HLA-only probe cannot recover a locked chip. Verify the probe
    firmware BEFORE any `mass_erase`.
- SWD wiring: the Bangle.js 2 charge cable exposes SWD on its USB-A plug.
  Remove all tape and adhesive from the cable pogo pins. Wire three pins
  only:

  | USB-A pin | Signal | Connect to ST-Link |
  |---|---|---|
  | 1 | 5V | Do not connect (charge the watch separately) |
  | 2 | SWDIO | SWDIO |
  | 3 | SWDCLK | SWDCLK |
  | 4 | GND | GND |

  The pads expose SWDIO, SWDCLK, and GND only. There is no SWO.
- Start at a modest adapter speed (1000-4000 kHz). Clones are unreliable
  at 10 MHz.

Connect and sanity-check:

```sh
openocd -f interface/stlink-dap.cfg -f target/nrf52.cfg \
  -c "adapter speed 2000" -c "init" -c "targets" -c "shutdown"
```

## APPROTECT gate — read FIRST, before anything writes

The watch ships with APPROTECT disabled (confirmed by the vendor). The
backup reads in this runbook work on a stock watch. The danger comes
AFTER an erase. Read the silicon revision from FICR before any
`mass_erase` decision:

```
mdw 0x10000104        ;# FICR INFO.VARIANT
```

Decode the value against the current nRF52840 Product Specification and
Nordic IN-133. Record this unit's VARIANT value in your notes.

- Rev 3+ ("improved APPROTECT", `Fx0` build codes): the factory default
  is PROTECTED. After a UICR erase or mass erase, you MUST write
  `UICR.APPROTECT = 0x5A` (HwDisabled) BEFORE any reset. A rev 3+ part
  that resets with an erased UICR comes up protected.
- Rev 1/2 ("old APPROTECT"): an erased UICR (`0xFFFFFFFF`) means
  DISABLED. Do NOT write `0x5A` there — skip the write entirely. On old
  silicon, any PALL value other than `0xFF` ENABLES protection. Recovery
  then needs CTRL-AP ERASEALL over a DAP-direct probe. With an HLA-only
  probe the chip is effectively bricked.

Get the revision right first. The write is mandatory on rev 3+ and
forbidden on rev 1/2.

## Flash procedure (ordered — do not reorder)

1. **SWD backup FIRST (read-only).** This is the brick insurance. Dump
   the stock internal 1 MiB and the UICR before any erase:

   ```sh
   openocd -f interface/stlink-dap.cfg -f target/nrf52.cfg \
     -c "adapter speed 2000" -c "init" -c "halt" \
     -c "dump_image bangle2_internal_backup.bin 0x0 0x100000" \
     -c "dump_image bangle2_uicr_backup.bin 0x10001000 0x400" \
     -c "resume" -c "shutdown"
   ```

   Store the dumps with checksums (`shasum -a 256 *.bin`), off-device.
2. **External storage backup.** Use the Bangle App Loader: More ->
   Backup. This preserves the user's data across step 3's full-chip
   erase. This step is mandatory.
3. **BLE full-chip external erase + pbpack preload (Espruino still
   runs).** Erase the ENTIRE 8 MiB external flash. Then upload
   `build/system_resources.pbpack` in chunks to Espruino address
   `0x60200000` (external offset `0x200000`) via the Espruino `Flash`
   module (`require("Flash")`). Verify each chunk with
   `E.CRC32(Flash.read(...))`. Then run a full-image CRC pass.

   The full-chip erase is belt-and-suspenders, not load-bearing: PFS
   formats a garbage FILESYSTEM, and the MFG_INFO/PFS readers tolerate
   blank-or-garbage content (pinned by the Renode `fs_garbage`
   scenario). Keep the erase anyway. Stale Espruino residue makes
   first-boot triage noisy.

   ```{warning}
   This step is UNVALIDATED ON HARDWARE. The exact Espruino erase API,
   the Flash-API behavior across the full bank, and the throughput are
   unproven (see Known limitations). Confirm the CRC pass before you
   continue.
   ```
4. **SWD-halt immediately after the final CRC.** Do not reboot Espruino.
   Do not launch an app. Espruino storage compaction could corrupt the
   freshly written bank.
5. **Program internal + boot** (in the OpenOCD console, target halted):

   ```
   program build/pebbleos.hex verify
   reset run
   ```

   `program` uses sector erase. NEVER use `mass_erase` in the standard
   path. The UICR is never erased here, so the improved-APPROTECT trap
   cannot arise.
6. **Verify.** `program ... verify` reports the verification result.
   After `reset run`, the watch boots PebbleOS. For a blank screen, use
   the triage list below.

## First-boot triage (blank screen)

Five failure classes look identical from the outside. Check over SWD in
this order (full table: design doc §13):

1. Wrong link base (`readelf -lW build/pebbleos.elf`: the lowest LOAD
   must be `0x0`).
2. WDT reset loop (RESETREAS at `0x40000400`, DOG bit; WDT RUNSTATUS at
   `0x40010400`).
3. Pbpack missing or mismatched (sad watch; read the dbgserial RAM
   buffer over SWD).
4. External garbage residue (non-`0xFF` data in regions the preload
   never wrote).
5. Genuine driver hang (halt; backtrace; stable PC in a driver poll
   loop).

A wedge BEFORE the firmware's watchdog self-arm hangs forever. That
window is milliseconds wide and SWD-recoverable. A wedge after the arm
resets within 8 s and shows in RESETREAS.

## mass_erase policy (recover / full clean only)

1. Gate on FICR first (see the APPROTECT gate above).
2. `nrf5 mass_erase`
3. On rev 3+ silicon ONLY, restore APPROTECT before any reset or power
   cycle.

   PRIMARY method: the OpenOCD `nrf5` driver maps the UICR as a flash
   bank and manages NVMC WEN/READY itself. Confirm the installed
   OpenOCD supports this against its `nrf5` driver docs before first
   use:

   ```
   flash fillw 0x10001208 0x0000005A 1
   ```

   FALLBACK method: the raw NVMC sequence, target halted. Registers
   confirmed against `nrf52840.h` (NVMC at `0x4001E000`: READY at
   `+0x400`, CONFIG at `+0x504`; UICR.APPROTECT at `0x10001208`):

   ```
   mww 0x4001E504 1           ;# NVMC.CONFIG = WEN (write enable)
   mdw 0x4001E400             ;# poll NVMC.READY until it reads 1
   mww 0x10001208 0x5A        ;# UICR.APPROTECT = 0x5A (HwDisabled)
   mdw 0x4001E400             ;# poll NVMC.READY == 1 (write completed)
   mww 0x4001E504 0           ;# NVMC.CONFIG = REN (back to read-only)
   ```

   The order is brick-critical: after the erase, BEFORE any reset.

## Restore to stock Espruino

Run `nrf5 mass_erase` (apply the policy above), then program a full
restore hex:

```sh
openocd -f interface/stlink-dap.cfg -f target/nrf52.cfg \
  -c "adapter speed 2000" -c "init" -c "halt" \
  -c "nrf5 mass_erase" \
  -c "program espruino_2v25_banglejs2.hex verify" \
  -c "reset run" -c "shutdown"
```

Then long-press the button at boot for the recovery menu and select
"Factory Reset". The Espruino hex carries the UICR NRFFW words;
`target/nrf52.cfg` defines the UICR bank, so `program` writes them.

Restore image sources, in order of preference:

1. The step-1 SWD dumps (`bangle2_internal_backup.bin` +
   `bangle2_uicr_backup.bin`) — the ground truth for this unit.
2. The staged full restore hex (MBR + SoftDevice + bootloader + Espruino
   2v25 app + UICR; contents verified complete):
   `~/.local/spellbook/docs/Users-eek-Development-PebbleOS/restore/espruino_2v25_banglejs2_FULL_restore.hex`

Cable-free restore is gone by design. Every restore needs the SWD rig.

## Brick insurance

- Keep the step-1 SWD backups (internal + UICR) with checksums,
  off-device.
- Verify probe firmware >= V2J24 before the FIRST `mass_erase`, not
  after.
- Never reset between a UICR erase and the APPROTECT restore write.
- Record the unit's FICR VARIANT value the first time you read it.

## Known limitations

These limitations are by design at `0x0` (design doc §16). There is no
PRF (recovery firmware) and no bootloader on the watch.

- **Factory reset does not complete its wipe.** The factory-reset path
  formats PFS and requests a PRF boot. No PRF exists. The watch reboots
  into the same firmware over a freshly formatted PFS. The "first use"
  hand-off that a PRF would perform never happens.
- **Recovery-mode entry is non-functional.** The button-hold recovery
  path and the phone-triggered recovery path both set the recovery boot
  bit and reset. No bootloader consumes the bit. The watch reboots into
  the same firmware. Recover from a bad state via SWD, not via on-watch
  recovery.
- **Deferred work:** PRF at `0x0`; a RAM-resident flashloader for fast
  external-flash writes over SWD; on-hardware validation of the BLE
  pbpack preload (step 3 above).
