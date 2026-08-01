# PebbleOS

PebbleOS is the operating system running on Pebble smartwatches.

## Organization

- `docs`: project documentation
- `resources`: firmware resources (icons, fonts, etc.)
- `sdk`: application SDK generation files
- `src`: firmware source
- `tests`: tests
- `third_party`: third-party code in git submodules, also includes glue code
- `tools`: a variety of tools or scripts used in multiple areas, from build
  system, tests, etc.
- `tools/libs`: Python packages used in multiple areas, e.g. log dehashing,
  console, etc.
- `tools/waf`: scripts used by the waf build system

## Code style

- clang-format for C code
- ruff for Python code
- Keep code comments short and concise. Extended descriptions can be kept in
  the Git commit message.
- Do not put references to issues in the code, only add those to the Git commit message.

## Logging

- `PBL_LOG_WRN` / `PBL_LOG_ERR` are for warnings and errors — use them as
  the names suggest.
- Default to `PBL_LOG_DBG` for routine lifecycle / state-transition logs.
  Reserve `PBL_LOG_INFO` for events that genuinely warrant attention in a
  default-level log capture; if a code path can fire repeatedly under
  normal use (e.g. play/pause spam, frequent state changes), it must not
  log at INFO.

## Firmware development

- Configure: `./pbl configure --board BOARD_NAME`

  - Board names can be obtained from `./pbl --help`
  - `-DCONFIG_RELEASE=y` enables release mode
  - `-DCONFIG_MFG=y` enables manufacturing mode
  - `--variant=normal|prf` selects build variant (default: normal)

- Build firmware: `./pbl build`
- Run tests: `./pbl test`

### Read the guards, not just the rows

Two sessions independently made the same wrong claim about the same table
within an hour: both read its rows and neither read the `#if defined(...)`
around them. The rows described behavior that is compiled out on this board.

"Read the table" and "read the guards around the table" feel like one act and
are two. The same shape appears without a preprocessor: a function body read
without its early returns, a config value read without the conditional that
overrides it. The qualifier always sits somewhere the eye treats as scaffolding
rather than content.

Before you claim a table, an enum, or a list does something, check what
compiles it in.

## Adding a new SDK function

When exposing a new function to third-party apps (i.e. anything declared
in an `applib/` header that user apps can call), three things must change
together — the firmware build alone won't surface it to apps:

1. **Implement the applib wrapper and syscall** — add the function to the
   appropriate `src/fw/applib/.../<area>.c/.h`, declare the syscall in
   `src/fw/syscall/syscall.h`, and define it with `DEFINE_SYSCALL` in
   `src/fw/syscall/syscall_<area>.c`.
2. **Register the symbol** in
   `tools/generate_native_sdk/exported_symbols.json` under the matching
   group, with an `addedRevision` matching the new SDK revision.
3. **Bump the SDK revision** in
   `src/fw/process_management/pebble_process_info.h`: increment
   `PROCESS_INFO_CURRENT_SDK_VERSION_MINOR` and add a `// sdk.major:0xN
   .minor:0xM -- <description> (rev <N+1>)` comment line above the
   `#define`. The revision number in the comment must match
   `addedRevision` from step 2.

Forgetting steps 2 or 3 means the function compiles into the firmware
but is invisible to the app SDK build, so third-party apps can't link
against it.

## Bangle.js 2 port (branch `bangle2-board`)

This branch ports PebbleOS to a physical Bangle.js 2 (nRF52840, SMA-Q3).
The full status table, load-bearing constraints, and ordered plan are in
`~/.local/spellbook/docs/Users-eek-Development-PebbleOS/roadmap/bangle2-port-status-and-roadmap-2026-07-26.md`
(read it first). Deep touch knowledge is in
`.../plans/2026-07-26-bangle2-touchscreen-mode-handoff.md`.

Two warnings about that roadmap document, recorded here because it lives
outside the repo and these corrections do not travel with a clone:

- **It is stale on BLE, in a way that invites wasted work.** It predates the
  bench validation: its subsystem table still shows BLE as STUBBED, and its
  R1 text still says a real nRF RNG driver is required before pairing can
  ship. Both are false. Decision D1 and bench result V2 established that the
  NimBLE controller owns `NRF_RNG`, that `CONFIG_RNG_STUB=y` pairs
  correctly, and that adding an RNG driver would CONTEND for the peripheral.
  Do not write one. Where that document and the R0-R10 checklist below
  disagree, the checklist wins.
- **`R10` means two different things.** Here and on every cross-session
  channel, R10 is "Bootloader + bootable PRF". In that roadmap document R10
  and R11 were "shake to wake" and "full display size"; they were renumbered
  to R12 and R13 on 2026-07-28, with R11 left unused to avoid a second
  collision. Write `R10 (bootloader+PRF)` rather than bare `R10` until the
  numbering is unified.

Load-bearing constraints (do not violate): bare-metal at flash `0x0` (no
MBR, no SoftDevice, no bootloader, no PRF); resources on external SPI-NOR
at `0x200000` and must match the build; SWD is the only delivery path (BLE
DFU of custom firmware is impossible); never run `mass_erase`,
`nrf52_recover`, or write UICR/FICR on the watch; the agent never pushes.

On `UICR.APPROTECT` specifically — this rule inverts the guidance you will
find for newer nRF52 parts, so it explains itself rather than only
prohibiting. The bench unit is **old-APPROTECT silicon**: measured over SWD
2026-07-30, `FICR INFO.VARIANT` = `0x41414430` (ASCII `AAD0`, an `Axx`
build code, not `Fx0`), with `UICR.APPROTECT` = `0xFFFFFFFF` (erased) while
the debug port was fully functional — which is only possible on rev 1/2.
On rev 3+ "improved APPROTECT" parts an erased UICR means PROTECTED, and
you must write `UICR.APPROTECT = 0x5A` after an erase to keep the port
open. **Here that is backwards: the erased state is the safe state, and
writing `0x5A` ENABLES protection and permanently kills SWD** — the only
recovery channel this board has. So: never write `UICR.APPROTECT` at all,
and treat any procedure calling `0x5A` a "disable" value as rev 3+ guidance
that does not apply. The firmware performs no run-time unlock that would
change this: `APPROTECT` appears exactly once in the tree, as a comment in
`src/fw/startup/startup_cortex_m.c`. Full detail and the measured register
table: `docs/boards/bangle2/index.md`, "APPROTECT gate".

Also note that external SPI-NOR has **zero backup coverage and cannot get
any from a debug probe** — the part is not memory-mapped on this chip, so
no probe reaches it regardless of quality. The BLE bond database
(`gap_bonding_db` in `FLASH_REGION_FILESYSTEM`, mirrored into
`FLASH_REGION_SHARED_PRF_STORAGE`) and the installed PRF image both live
there. Only a custom flashloader driving SPIM2 from RAM can ever read or
write that part, and it does not exist yet. Plan flashing decisions on the
assumption that everything on external NOR is unrecoverable if lost.

On PRF specifically: its absence is recorded in `boards/bangle2/Kconfig`
(`config PRF_UNAVAILABLE`) — the blocker is the missing bootloader (nothing
selects PRF vs normal firmware at reset; "a PRF reset lands in the same image
forever"), NOT a decision against PRF. The flash map already reserves a 512 K
`SAFE_FIRMWARE` region (`flash_region_bangle2.h:29`). This is the root of the
official app's recovery-mode gate: the app forces recovery mode when the watch
reports `recoveryFwVersion=null`, which skips `blobDB.init()` and blocks
notifications. See R1 (near-term metadata fix) and R10 (bootloader + PRF).

### Roadmap checklist (ordered)

- [x] **R0 — Commit the touch bring-up pile.** Done: recovery blob,
  de-shear recognizer + touchlog, 5 s-hold reset, atomic synthetic clicks.
  Pushed to `elijahr/PebbleOS` fork only.
- [~] **R1 — BLE bring-up (VALIDATED on hardware 2026-07-27; follow-ups open).**
  NimBLE host+controller enabled (bare-metal on RADIO, no SoftDevice) via the
  configure-time split `-DCONFIG_BT_FW_NIMBLE=y` (D2: mainline defconfig keeps
  the stub, CI job `387a1920` compiles the silicon config). Durable code change:
  PPI CH4-7 reservation (`ad0a673b`). NO RNG driver needed (D1 — the NimBLE
  controller owns `NRF_RNG`; `CONFIG_RNG_STUB=y` pairs fine; a driver would
  contend for the peripheral). Bench: V1 boot+host-sync PASS (5/5), V2
  pairing+bond+PPoGATT+Pebble Protocol PASS, V3 time sync PASS. Full evidence:
  `~/.local/spellbook/docs/Users-eek-Development-PebbleOS/plans/2026-07-27-bangle2-ble-bench-validation-log.md`.
  Open follow-ups: notifications blocked by the recovery-mode/`recoveryFwVersion=null`
  gate (near-term fix in progress on branch `bangle2-notifications`; durable fix
  = R10); V4 robustness + V5 coexistence; platform-22 `UNKNOWN`/FW-update
  cosmetics (register BANGLE2(22) upstream in `libpebble3`). R3 (tickless-idle
  stall) has a fix written, but it is NOT BUILT and NOT BENCH-VERIFIED. Build it
  before you start power work. The one-off `0x11` assert
  seen during a failed re-pair is
  ROOT-CAUSED AND FIXED: `bt_driver_advert_advertising_disable` checked
  `ble_gap_adv_active()` without the host lock, then asserted on any non-zero
  from `ble_gap_adv_stop()`, which re-checks under the lock and reports
  `BLE_HS_EALREADY` when advertising stopped in between — a real two-task race,
  since that runs on the comm task while the NimBLE host task stops advertising
  by itself on connect, disconnect, or pairing timeout. `BLE_HS_EALREADY` and
  `BLE_HS_EDISABLED` are now treated as success, since both mean advertising is
  already off, which is what the call wanted.
- [ ] **R2 — Display white-border / top-cutout anomaly.** Border constant is
  BLACK yet renders white → suspect 3bpp polarity / bit-reversal in encode,
  or Y-offset off-by-one. Bench debug.
- [ ] **R3 — Tickless-idle time stall (FIX WRITTEN, UNBUILT; was a REGRESSION).**
  STATUS: the change below is source-reviewed only. Nobody has compiled it —
  `arm-none-eabi-gcc` was not installed on the machine where it was written, and
  CI does not build this branch (see the `branches: [main]` trigger gap). Build
  it before you trust it. Commit
  `011bb5a8d` ("soc/nrf52: cleanup sleep code") deleted
  `MAX_STOP_TICKS = RTC_TICKS_HZ` and the clamp
  `MIN(xExpectedIdleTime - EARLY_WAKEUP_TICKS, MAX_STOP_TICKS)` in
  `vPortSuppressTicksAndSleep`. Fix restores both. Do NOT add an assert on
  `num_ticks` in `rtc_alarm_set`: `PBL_ASSERTN` is NORETURN (reboots via
  `trigger_fault`), and `num_ticks >= 2^24` is reachable from legitimate
  FreeRTOS input (empty delayed-task list gives
  `xExpectedIdleTime = portMAX_DELAY - xTickCount`) — an assert there is a
  reboot loop, not a fix. A clamp at the 24-bit hardware limit also does not
  fix the symptom: the wait register masks to `num_ticks mod 2^24`, so a
  worst-case request already yields ~2^24 ticks (~4.55 h); clamping at 2^24
  changes that by ~3 ticks. `RTC_TICKS_HZ` (1 s) is the correct, restored
  clamp, and matches the timer service's 1000 ms repeating callback. Also
  fixed on the same path: `vTaskStepTick`'s argument is now bounded by
  `xExpectedIdleTime` (`rtc_alarm_get_elapsed_ticks()` is free-running real
  time and can exceed the requested sleep; `vTaskStepTick`'s `configASSERT`
  is the same NORETURN `PBL_ASSERT`). The stall was latent, not live: RTC
  COMPARE_1 keeps feeding the task watchdog every 500 ms through
  `rtc_systick_pause()`, and PRIMASK does not block WFI wake, so the CPU
  wakes every ~500 ms regardless of CC0; the 8 s hardware watchdog is a
  second ceiling. This is why R3 had to land before power work: the first
  obvious power optimization is suppressing that 2 Hz feed, which makes the
  stall live. Known gap left open: `rtc_alarm_set` still has no driver-side
  upper clamp, so the 24-bit constraint is enforced only by the caller;
  left out on purpose since it changes a public API and wants its own
  review.
- [ ] **R4 — Touch Phase 2: absolute tap calibration.** N-point affine
  tap-grid fit (chip coords → 176x176). Do NOT revert the atomic one-shot
  click model. Drag-to-scroll can ship first.
- [ ] **R5 — EXTI input-buffer audit.** `exti_configure_pin` passes
  `p_pull_config=NULL`; central fix connects the input buffer. bangle2 is
  safe now; shared `lsm6dso` / `npm1300` callers are exposed (upstream
  hygiene).
- [ ] **R6 — Renode / test debt.** Auto-regenerate `build_id_restore.resc`
  after every build; parameterize hardcoded paths; add bangle2 to CI.
  Decide keep-vs-gate for the 256-entry touchlog ring buffer.
- [ ] **R7 — Charger wake + charge-pin semantics.** Add GPIO SENSE wake for
  System OFF; fill the P0.23/P0.25 truth table; fix the incidental
  `nrf_rtc_event_enable` offset-vs-mask misuse.
- [ ] **R8 — Silicon-proof the remaining PARTIALs.** Battery curve, accel
  shake/tap, mag calibration, backlight brightness. GPS and native 176x176
  color platform stay PAUSED until reopened.
- [ ] **R9 — Upstream prep.** Fix `git config user.email` (gitlint UC3
  rejects the `noreply` address) + rewrite branch identity; SPDX + DCO `-s`
  + `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` on every
  commit, no issue refs; run `gitlint` + `clang-format`; resolve the
  touchlog gate. DO NOT PUSH — the operator pushes upstream.
- [ ] **R10 — Bootloader + bootable PRF (recovery infrastructure).** Build
  boot-selection at reset (PRF vs normal firmware) + a PRF firmware image in the
  reserved `SAFE_FIRMWARE` region (`flash_region_bangle2.h:29`). Unblocks
  over-BLE recovery (un-brick without SWD), safe OTA updates, and the official
  app's recovery-mode gate honestly (non-null `recoveryFwVersion`); removes
  `PRF_UNAVAILABLE`. Owner: pebble-bootloader (own worktree/branch off
  `bangle2-board`). NOTE the near-term stopgap that does NOT need a bootloader:
  report valid recovery FW metadata so the app exits recovery mode (owner:
  pebble-noti, branch `bangle2-notifications`) — must degrade to a stable state
  per `PRF_UNAVAILABLE` (there is no real PRF to boot into).

## Git rules

Main rules:

- Commit using `-s` git option, so commits have `Signed-Off-By`
- Always indicate commit is co-authored by the current AI model
- Commit in small chunks, trying to preserve bisectability
- Commit format is `area: short description`, with longer description in the
  body if necessary
- Run `gitlint` on every commit to verify rules are followed

Others:

- If fixing Linear or GitHub issues, include in the commit body a line with
  `Fixes XXX`, where XXX is the issue number.
