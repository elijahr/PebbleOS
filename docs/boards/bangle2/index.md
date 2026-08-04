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

> **This unit is OLD-APPROTECT silicon. NEVER write `UICR.APPROTECT`.**
>
> Measured over SWD on 2026-07-30, so you do not have to decode anything
> mid-restore:
>
> | Register | Address | Value |
> |---|---|---|
> | `FICR INFO.PART` | `0x10000100` | `0x00052840` (nRF52840) |
> | `FICR INFO.VARIANT` | `0x10000104` | `0x41414430` = ASCII `AAD0` |
> | `UICR.APPROTECT` | `0x10001208` | `0xFFFFFFFF` (erased) |
>
> `AAD0` is an `Axx` build code, not `Fx0`: this is the **rev 1/2 old
> APPROTECT** case below. The debug port was fully functional at the same
> moment the APPROTECT word read erased, which is only possible on old
> silicon — on rev 3+ an erased UICR means PROTECTED. The firmware performs
> no run-time unlock that could explain it otherwise: the string `APPROTECT`
> appears exactly once in the whole tree, as a comment in
> `src/fw/startup/startup_cortex_m.c`, and no code writes either
> `UICR.APPROTECT` or the `APPROTECT.DISABLE` register.
>
> Therefore, on this watch, `mww 0x10001208 0x5A` is the brick command.
> Writing `0x5A` ENABLES protection here. Skip that step entirely. The
> erased state is the safe state. If someone hands you a procedure that
> says `0x5A` is the "disable" value, they are reading rev 3+ guidance and
> it is inverted for this unit.

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
forbidden on rev 1/2. For the bench unit this is settled: VARIANT `AAD0`,
old APPROTECT, so the write is FORBIDDEN.

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

The subsections below extend the triage list. Do step 0 before you
flash. Do step 1 first at the bench. Use the other subsections when the
matching symptom appears.

```{note}
Build the bring-up image at a debug log level
(`-DCONFIG_DEFAULT_LOG_LEVEL_DEBUG=y`). The default level is INFO.
INFO-level builds compile out every `PBL_LOG_DBG` line. The UICR
visibility line and the charge-pin lines below are `PBL_LOG_DBG` lines.
A release-level build never shows them.
```

### Triage step 0: UICR state check (before flash, after first boot)

Dump the UICR twice: once before you flash, once after the first boot.
Read the words over SWD (OpenOCD console, target halted):

```
mdw 0x10001200 2      ;# PSELRESET[0], PSELRESET[1]
mdw 0x10001208        ;# APPROTECT
mdw 0x1000120C        ;# NFCPINS
mdw 0x10001304        ;# REGOUT0
```

Interpret the pre-flash dump with this table. The dump predicts exactly
which write+reset cycles fire on the first boot. The NFCPINS guard runs
in SystemInit. The REGOUT0 guard runs in Reset_Handler
(`startup_cortex_m.c`, after SystemInit returns):

| Word | Value | Meaning | First-boot action |
|---|---|---|---|
| PSELRESET[0..1] | `0xFFFFFFFF` | Erased; no reset pin | None. `CONFIG_GPIO_AS_PINRESET` is dropped for bangle2. The write block does not exist in the binary. |
| NFCPINS | `0xFFFFFFFE` | GPIO mode (Espruino-programmed) | None. The guard skips. |
| NFCPINS | `0xFFFFFFFF` | NFC mode (erased default) | One write + reset cycle. |
| REGOUT0 | `0xFFFFFFFD` | VOUT = 5 = 3.3 V (Espruino-programmed) | None. The guard skips. |
| REGOUT0 | `0xFFFFFFFF` | Erased default (1.8 V) | One write + reset cycle. The guard programs VOUT = 5. |
| APPROTECT | Open (disabled) | Debug port open | None. The firmware never writes APPROTECT. See the APPROTECT gate above. |

Known state of THIS watch (pre-flash dump
`restore/bangle2-eek-stock-uicr-2026-07-24.bin`, spellbook docs tree):
REGOUT0 = `0xFFFFFFFD` (3.3 V already), NFCPINS = `0xFFFFFFFE` (GPIO),
PSELRESET[0..1] = `0xFFFFFFFF` (erased), APPROTECT open. Expected first
boot on this watch: ZERO UICR writes. The post-boot dump must be
byte-identical to the pre-flash dump. Compare with `diff` on two SWD
dumps of `0x10001000 0x400`.

The guard-programmed-from-default REGOUT0 value is exactly
`0xFFFFFFFD`. It is not a `0x...F5` variant. If REGOUT0 reads a value
that is not 3V3-coded and not the erased default: stop. Fix it over SWD
per the mass_erase policy. The firmware does not touch it.

After a `mass_erase` (fully erased UICR), expect up to TWO write+reset
cycles on the next boot: NFCPINS first, then REGOUT0. Then re-read
REGOUT0 and verify VOUT = 5.

Visibility: `main.c` logs `UICR REGOUT0=0x...` once at boot. This line
is `PBL_LOG_DBG` (see the log-level note above).

### Triage step 1: verify the debug UART pads

TX is P1.11 (UATX pad). RX is P1.10 (UARX pad). This pin claim has a
single source (gfwilliams/pebble-banglejs2). It stays UNVERIFIED until
this step passes. Connect a 3.3 V UART adapter to the pads. Run:

```sh
python tools/pulse_console.py -t /dev/ttyUSB0
```

Boot the watch. Console output proves the pads. No output means: wrong
pads, or a boot wedge before the first log line. Halt over SWD and read
the PC to tell the two apart. Every later triage step reads this
console, so do this step first.

### Display fallback: 2 MHz clock drop (documented only)

No code change ships for this item. The shipped tree keeps the 4 MHz
SPIM clock. Apply the fallback at the bench ONLY if the display
misbehaves (garbage, ghosting, no image):

1. Edit `src/fw/drivers/display/lpm013m126/lpm013m126_nrf5.c:269`.
   Change `config.frequency = NRFX_MHZ_TO_HZ(4);` to
   `NRFX_MHZ_TO_HZ(2)`. 2 MHz is Gordon's hardware-proven clock.
2. Rebuild. Reflash. Retest.

This fallback is a clock drop only. It is NOT a 1-bit-mode rebuild.
1-bit mode uses a different update command and a different framebuffer
format. 1-bit mode is out of scope.

### Button polarity falsification test

The shipped config is internal PULLUP, active-low. The line idles HIGH.
A press reads electrically LOW. This matches effective Espruino behavior
(the NEGATED pin flag) and Gordon's port. An earlier fork-comparison
report stated the opposite; that report now carries an erratum.

Confirm on hardware: press the button and watch for the press event on
the console. Correct polarity shows one press event per press, and no
events at idle.

Wrong polarity looks like one of these two symptoms:

- The button reads stuck-pressed from boot: constant or repeated press
  events with no touch.
- The button reads dead: no events on press.

The three phantom button slots are parked on `GPIO_Pin_NULL`, and the
drivers skip NULL pins. A floating pad can no longer fake a
stuck-pressed button. If a stuck or dead button appears, first apply
the falsification test: change the P0.17 slot to PULLDOWN/active-high
locally, rebuild, and retest. If the button then works, the active-low
claim was wrong — record that result. If the button fails both ways,
suspect wiring, not polarity.

### Charge-pin truth table (P0.23 vs P0.25)

The shipped driver reads two pins. P0.23 is the sole authority
(Espruino semantics: LOW = charging). P0.25 is a logged observer only.
It feeds no decision. The true semantics stay UNRESOLVED until this
observation: Espruino semantics say P0.23 = charging; Gordon semantics
say P0.23 = USB present and P0.25 = charging. Gordon's note against
interrupt-driven charge sensing is hedged ("can cause instability?") —
a question, not a confirmed claim. Both pins stay poll-only regardless.

Fill this table at the bench from the
`charge pins: P0.23=... P0.25=...` log lines (`PBL_LOG_DBG`; see the
log-level note above). The line prints only on change.

| USB | Battery | P0.23 | P0.25 |
|---|---|---|---|
| Plugged | Not full (charging) | ? | ? |
| Plugged | Full (charge complete) | ? | ? |
| Unplugged | Not full | ? | ? |
| Unplugged | Full | ? | ? |

Decode: if P0.23 goes HIGH at charge-complete while USB stays plugged,
P0.23 means "charging" (Espruino semantics). If P0.23 stays LOW
whenever USB is plugged, P0.23 means "USB present" and P0.25 is the
charging line (Gordon semantics). Update
`src/fw/drivers/battery/battery_bangle2.c` per the observed table. This
table is the one the driver comment points at.

### JEDEC flash-id triage

The boot log names the external-flash part. The classifier has three
tiers:

| Tier | Log line | Boot behavior |
|---|---|---|
| Known id | `SPI-NOR flash GD25Q64E detected (JEDEC 0x001740c8)` or `SPI-NOR flash XT25F64B detected (JEDEC 0x0017400b)` | Normal boot. INFO line only. |
| Unknown 8 MB | `UNKNOWN-8MB SPI-NOR (JEDEC 0x...): right capacity, unknown vendor; ...` | Warn, then boot. Verify the part. Update `bangle2_flash_ids.h`. |
| Dead bus / rejected | `SPI-NOR JEDEC id 0x... DEAD-BUS -- refusing to boot ...` or `... REJECTED -- refusing to boot ...` | Fail-loud croak. The watch does not boot. |

Read the raw id from the log line. `0x000000` or `0xFFFFFF` means a
dead bus (wiring, power, or CS problem). Any other rejected id means a
real part answered with a wrong capacity — check the fitted chip. The
XTX manufacturer byte `0x0B` is CONFIRMED (flashrom defines
`XTX_ID = 0x0B`). Only the attribution of the XT25F64B id to Gordon's
hardware correction stays inferred; a real `0x0017400B` read here
confirms that too.

## mass_erase policy (recover / full clean only)

1. Gate on FICR first (see the APPROTECT gate above).
2. `nrf5 mass_erase`
3. On rev 3+ silicon ONLY, restore APPROTECT before any reset or power
   cycle.

   > **NOT ON THIS WATCH.** The bench unit measured `AAD0` on 2026-07-30
   > and is old-APPROTECT silicon (see the APPROTECT gate above). Writing
   > `0x5A` here ENABLES protection and permanently kills SWD — the only
   > recovery channel this board has. SKIP this entire step. Go straight
   > from `mass_erase` to programming. The commands below are for rev 3+
   > parts only; do not run them on this unit.

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
   ;# DO NOT RUN THE NEXT LINE ON THE BENCH UNIT (FICR VARIANT AAD0, old
   ;# APPROTECT): there this write ENABLES protection and permanently
   ;# kills SWD. Rev 3+ silicon only. Verify VARIANT for the unit in
   ;# front of you first.
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

1. **Your own step-1 SWD dumps.** Use `bangle2_internal_backup.bin` and
   `bangle2_uicr_backup.bin` from step 1 above. This is the ground truth
   for your unit. Restore from these dumps for a byte-exact result.
2. **A generic stock Espruino image.** Use this only if you have no
   step-1 dump for this unit, for example on a first restore of a watch
   you did not dump yourself. Get the image one of two ways:

   - Download it from Gordon Williams' Bangle.js 2 port repo:
     [gfwilliams/pebble-banglejs2](https://github.com/gfwilliams/pebble-banglejs2).
     Use the file `banglejs/espruino_2v25_banglejs2.hex`. This file is a
     complete image: MBR, SoftDevice, bootloader, and Espruino app.
   - Or build the image yourself from Espruino source:

     ```sh
     make BOARD=BANGLEJS2 RELEASE=1
     ```

     This command merges the SoftDevice, bootloader, and app into one
     hex file.

   Save the image next to your other build artifacts as
   `espruino_2v25_banglejs2.hex`. The restore command above uses this
   filename.

   (A local copy of this file may already be staged from earlier
   development work. Either source above reproduces it.)

Cable-free restore is gone by design. Every restore needs the SWD rig.

## Brick insurance

- Keep the step-1 SWD backups (internal + UICR) with checksums,
  off-device. Name every dump after the version string read OUT OF the
  dumped image, never after the version you believe was flashed — on
  2026-07-30 both pre-existing internal backups were checked against the
  running firmware and NEITHER matched, so either one would have restored
  the wrong image if reached for mid-flash.
- Verify probe firmware >= V2J24 before the FIRST `mass_erase`, not
  after.
- On rev 3+ silicon, never reset between a UICR erase and the APPROTECT
  restore write. **On this unit (old APPROTECT) there is no restore write**
  — an erased UICR already means DISABLED. Skip it entirely; do not go
  looking for the step.
- **This unit's FICR VARIANT is `AAD0` (old APPROTECT), measured
  2026-07-30.** The `UICR.APPROTECT = 0x5A` write is FORBIDDEN on it.
  Re-read VARIANT for any other unit before assuming the same.
- External SPI-NOR has NO backup and cannot get one from any debug probe,
  however good — the 8 MB part is not memory-mapped on the nRF52840, it
  hangs off GPIO driven by SPIM2. The BLE bonding store and the installed
  PRF image live there. Until a SPIM2 flashloader exists, treat everything
  on external NOR as unrecoverable if lost.

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
