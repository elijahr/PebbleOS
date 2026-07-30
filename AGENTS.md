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

Load-bearing constraints (do not violate): bare-metal at flash `0x0` (no
MBR, no SoftDevice, no bootloader, no PRF); resources on external SPI-NOR
at `0x200000` and must match the build; SWD is the only delivery path (BLE
DFU of custom firmware is impossible); never run `mass_erase`,
`nrf52_recover`, or write UICR/FICR on the watch; the agent never pushes.

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
  cosmetics (register BANGLE2(22) upstream in `libpebble3`); a one-off
  SM/pairing-timeout assert (`0x11`) to root-cause. Fix R3 before power work.
- [ ] **R2 — Display white-border / top-cutout anomaly.** Border constant is
  BLACK yet renders white → suspect 3bpp polarity / bit-reversal in encode,
  or Y-offset off-by-one. Bench debug.
- [ ] **R3 — Tickless-idle time stall.** Clamp `xExpectedIdleTime` to
  `RTC_TICKS_HZ * 60` in `vPortSuppressTicksAndSleep`; assert
  `num_ticks < (1<<24)` in `rtc_alarm_set`. Fix before any power work.
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
