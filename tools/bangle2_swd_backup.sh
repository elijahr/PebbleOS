#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0
#
# Bangle.js 2 read-only SWD inventory + backup.
#
# Every operation here is a READ. No erase, no program, no reset. The target is
# halted to read and resumed afterwards, in a single openocd session so the
# resume always runs. Nothing on the watch is modified.
#
# It exists because both pre-existing backups on this bench were mislabelled:
# their filenames named a firmware version that did not match the bytes inside.
# So this names each dump after the version string READ OUT OF the dumped
# image, never after the version someone believes was flashed, and writes a
# SHA256SUMS beside them.
#
# WHAT THIS DOES NOT COVER: the 8 MB external SPI-NOR. That part is not
# memory-mapped on the nRF52840 (it hangs off SPIM2 on plain GPIO), so no
# debugger can read it directly. Backing it up needs host-driven SPIM2 over
# SWD or a RAM-resident flashloader; see the R10 bootloader design doc.
#
# Usage: tools/bangle2_swd_backup.sh [outdir]
#   OOCD_IFACE  openocd interface cfg (default interface/stlink-dap.cfg)
#   OOCD_TARGET openocd target cfg    (default target/nrf52.cfg)
#   ADAPTER_SPEED_KHZ  SWD clock in kHz (default 2000)
#
# The default interface is an ST-Link/V2 in DAP mode, which is what this bench
# uses; the board's own openocd.cfg points at cmsis-dap.cfg and will not find
# that probe. Override OOCD_IFACE for a different probe.

set -euo pipefail

OUTDIR="${1:-$HOME/Development/bangle2-swd-backups}"
OOCD_IFACE="${OOCD_IFACE:-interface/stlink-dap.cfg}"
OOCD_TARGET="${OOCD_TARGET:-target/nrf52.cfg}"
ADAPTER_SPEED_KHZ="${ADAPTER_SPEED_KHZ:-2000}"

STAMP="$(date -u +%Y-%m-%dT%H%M%SZ)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Portable file size: BSD/macOS `stat -f%z` vs GNU/Linux `stat -c%s`.
filesize() {
  stat -f%z "$1" 2>/dev/null || stat -c%s "$1"
}

mkdir -p "$OUTDIR"

echo "== bangle2 SWD read-only inventory =="
echo "outdir: $OUTDIR"
echo "stamp:  $STAMP"
echo "probe:  $OOCD_IFACE @ ${ADAPTER_SPEED_KHZ} kHz"
echo

# ---- 1. Probe + FICR/UICR reads + the dumps, in ONE openocd session ---------
# One session keeps the halt window as short as possible and guarantees the
# resume runs even if a later read is added. The nRF52840 WDT is left at its
# reset-default pause-on-debug-halt config, so it stops counting while halted;
# a long dump is therefore safe against an armed watchdog.
echo "-- attaching (halt, read, resume) --"
openocd \
  -f "$OOCD_IFACE" \
  -f "$OOCD_TARGET" \
  -c "adapter speed ${ADAPTER_SPEED_KHZ}" \
  -c 'init' \
  -c 'halt' \
  -c 'echo "=== FICR INFO.PART @ 0x10000100 ==="' \
  -c 'mdw 0x10000100' \
  -c 'echo "=== FICR INFO.VARIANT @ 0x10000104 ==="' \
  -c 'mdw 0x10000104' \
  -c 'echo "=== FICR INFO.RAM/FLASH @ 0x1000010C 0x10000110 ==="' \
  -c 'mdw 0x1000010c 2' \
  -c 'echo "=== UICR APPROTECT @ 0x10001208 ==="' \
  -c 'mdw 0x10001208' \
  -c 'echo "=== UICR NFCPINS @ 0x1000120C ==="' \
  -c 'mdw 0x1000120c' \
  -c 'echo "=== UICR REGOUT0 @ 0x10001304 ==="' \
  -c 'mdw 0x10001304' \
  -c "dump_image $WORK/internal.bin 0x0 0x100000" \
  -c "dump_image $WORK/uicr.bin 0x10001000 0x400" \
  -c 'resume' \
  -c 'shutdown' 2>&1 | tee "$WORK/openocd.log"

echo
echo "-- verifying dump sizes --"
INT_SZ=$(filesize "$WORK/internal.bin")
UICR_SZ=$(filesize "$WORK/uicr.bin")
echo "internal: $INT_SZ B (expect 1048576)"
echo "uicr:     $UICR_SZ B (expect 1024)"
[ "$INT_SZ" -eq 1048576 ] || { echo "FAIL: internal dump wrong size"; exit 1; }
[ "$UICR_SZ" -eq 1024 ] || { echo "FAIL: uicr dump wrong size"; exit 1; }

# ---- 2. Name the files after what is INSIDE the image ------------------------
echo
echo "-- reading the version string out of the dump --"
VER="$(strings -a "$WORK/internal.bin" \
       | grep -oE 'v?[0-9]+\.[0-9]+\.[0-9]+-[0-9]+-g[0-9a-f]{7,}' \
       | sort -u | head -1 || true)"

if [ -z "$VER" ]; then
  echo "WARNING: no version string found in the dump."
  echo "Naming the files 'unknown-version'. Investigate before trusting them."
  VER="unknown-version"
else
  echo "version in image: $VER"
fi

BASE="bangle2_${VER}_${STAMP}"
cp "$WORK/internal.bin" "$OUTDIR/${BASE}_internal.bin"
cp "$WORK/uicr.bin"     "$OUTDIR/${BASE}_uicr.bin"
cp "$WORK/openocd.log"  "$OUTDIR/${BASE}_openocd.log"

# ---- 3. Checksums, appended so earlier entries are never lost ---------------
echo
echo "-- writing checksums --"
( cd "$OUTDIR" && shasum -a 256 \
    "${BASE}_internal.bin" "${BASE}_uicr.bin" >> SHA256SUMS )
( cd "$OUTDIR" && shasum -a 256 -c SHA256SUMS 2>&1 | tail -5 )

echo
echo "-- decoded UICR (from the fresh dump) --"
python3 - "$OUTDIR/${BASE}_uicr.bin" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
def w(off): return struct.unpack_from('<I', d, off)[0]
for name, off in [('PSELRESET[0]', 0x200), ('PSELRESET[1]', 0x204),
                  ('APPROTECT', 0x208), ('NFCPINS', 0x20C),
                  ('REGOUT0', 0x304)]:
    print(f'  {name:14s} @0x{off:03X} = 0x{w(off):08X}')
PY

echo
echo "== done. Device state UNCHANGED: nothing was written. =="
echo "Files:"
ls -l "$OUTDIR/${BASE}"* | awk '{print "  " $5, $9}'
