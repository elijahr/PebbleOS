#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

"""Build a Nordic Secure DFU application-only .zip for the Bangle.js 2, in pure
Python, structurally matched to the stock Espruino 2v27 package.

The bangle2 firmware runs as a bootloader-hosted DFU application at flash base
0x26000 (above the dormant SoftDevice). The stock Bangle.js 2 bootloader accepts
an application-only Secure DFU package. This tool produces that package -- an
`app.bin` (the contiguous application image), an `app.dat` (the dfu-cc init
packet), and a `manifest.json` -- without nrfutil. It hand-rolls the init packet
with the vendored dfu-cc schema (tools/dfu/dfu_cc_pb2.py) so any user can rebuild
it reproducibly with only the PebbleOS venv (protobuf + intelhex), no install of
the abandoned pc-nrfutil toolchain.

Structural fidelity is proven, not assumed: fed the stock golden's own
image-specific values, this tool's protobuf assembly reproduces the 146-byte
golden init packet (tools/dfu/golden/espruino_2v27_banglejs2_app.dat)
byte-for-byte (see tools/tests/test_make_dfu_package.py::test_assembly_
reproduces_golden_bytes). Every enumerated field is additionally checked
against the golden before the package is written, so a mis-built package
(wrong hardware, wrong SoftDevice requirement, wrong image type) is rejected
here rather than by the device.

Safety: the Espruino bootloader has signature verification disabled, so the
signature bytes are an inert valid-length (64-byte, ECDSA P-256) placeholder.
A structurally wrong or hash-mismatched package is REJECTED by the bootloader
(and by this tool's own checks); it cannot brick a device via this path (this
rests on the stock bootloader's own reject-and-recover behavior, confirmed by
the first hardware flash).

Flashing: load the produced .zip via the Bangle.js App Loader
"Firmware Update" (Advanced) flow. The first hardware flash is the final
acceptance confirmation.

Usage:
    make_dfu_package.py [<hex> [<out.zip> [<golden.dat>]]]
        Build and verify an app-only DFU package from <hex>
        (default build/pebbleos.hex, output pebbleos_banglejs2_app_dfu.zip).
    make_dfu_package.py --verify-dat <generated.dat> [<golden.dat>]
        Verify an existing init packet's enumerated fields against the golden.
"""

import hashlib
import json
import sys
import zipfile
from pathlib import Path

from intelhex import IntelHex

# The vendored, generated init-packet decoder lives next to this tool under
# tools/dfu/. Insert that directory (not a package import) so the module
# resolves regardless of the current working directory.
sys.path.insert(0, str(Path(__file__).resolve().parent / "dfu"))
import dfu_cc_pb2  # noqa: E402  (generated from tools/dfu/dfu_cc.proto)

# The app flash base = ORIGIN(FLASH) = address of __ISR_VECTOR_TABLE__ after the
# bangle2 relink. Every hex record must sit at or above this address; anything
# below would write the MBR, the dormant SoftDevice, or the bootloader.
APP_BASE = 0x26000

# Init-packet field values, matched to the stock Espruino 2v27 golden:
#   hw_version 52            Bangle.js 2 (nRF52840).
#   fw_version 255 (0xff)    Same-version DFU; the bootloader test is ">=", so
#                            0xff over 0xff is always accepted and a stock
#                            restore stays possible.
#   sd_req [0xa9,0xb6,0xae]  S140 6.0.0 / 6.1.1 / 6.1.0 FWIDs, in the golden's
#                            exact order (keep-SoftDevice path).
# The order of sd_req is replicated exactly so the packet is byte-identical to
# nrfutil's for the same image; the runtime field check compares it as a set.
HW_VERSION = 52
FW_VERSION = 255
SD_REQ = (0xA9, 0xB6, 0xAE)

# 64-byte ECDSA P-256 signature length, matched to the golden. Verification is
# disabled in the Espruino bootloader, so the bytes are an inert placeholder;
# only the structural length matters.
SIGNATURE_LEN = 64
PLACEHOLDER_SIGNATURE = b"\x00" * SIGNATURE_LEN

# Package member names (app-only Secure DFU).
BIN_NAME = "pebbleos_banglejs2_app.bin"
DAT_NAME = "pebbleos_banglejs2_app.dat"
MANIFEST_NAME = "manifest.json"

# Golden init packet vendored into the repo, referenced relative to this tool
# (never a session-ephemeral scratchpad). Overridable via the CLI for CI.
DEFAULT_GOLDEN_DAT = (
    Path(__file__).resolve().parent
    / "dfu"
    / "golden"
    / "espruino_2v27_banglejs2_app.dat"
)

# The enumerated fields that MUST match the stock golden. This is NOT a
# byte-compare: app_size and the app hash legitimately differ per image and are
# excluded. sd_req is compared as a set because the golden field check does not
# depend on the order of the SoftDevice requirement list.
REQUIRED_FIELDS = {
    "type": "APPLICATION",
    "sd_req": frozenset({0xA9, 0xAE, 0xB6}),
    "hw_version": 52,
    "sd_size": 0,
    "bl_size": 0,
}


class DfuPackageError(Exception):
    """Base error for a rejected DFU package."""


class RecordsBelowBaseError(DfuPackageError):
    """A hex record sits below the app base (would overwrite MBR/SD/bootloader)."""

    def __init__(self, min_addr, app_base):
        self.min_addr = min_addr
        self.app_base = app_base
        super().__init__(
            f"hex record at 0x{min_addr:x} is below app base 0x{app_base:x}"
        )


class FieldMismatchError(DfuPackageError):
    """The generated init packet differs from the golden on an enumerated field."""

    def __init__(self, mismatches):
        # mismatches: {field_name: (generated_value, golden_value)}
        self.mismatches = mismatches
        detail = ", ".join(
            f"{name}: generated={gen!r} golden={gold!r}"
            for name, (gen, gold) in sorted(mismatches.items())
        )
        super().__init__(f"init-packet field mismatch: {detail}")


class HashMismatchError(DfuPackageError):
    """The init packet's embedded hash does not describe the paired image."""

    def __init__(self, embedded, computed):
        self.embedded = embedded
        self.computed = computed
        super().__init__(
            f"init-packet hash {embedded.hex()} does not match image hash "
            f"{computed.hex()}"
        )


def _image_hash(app_bin):
    """Return the Nordic-convention image hash: SHA256 in little-endian
    (byte-reversed) order, exactly as nrfutil embeds it."""
    return hashlib.sha256(app_bin).digest()[::-1]


def records_in_range(hex_path):
    """Return the minimum record address in the hex. Raise RecordsBelowBaseError
    if any record is below the app base."""
    ih = IntelHex(hex_path)
    min_addr = ih.minaddr()
    if min_addr < APP_BASE:
        raise RecordsBelowBaseError(min_addr, APP_BASE)
    return min_addr


def extract_app_bin(hex_path):
    """Return the contiguous application image from APP_BASE to the top of the
    hex, padding gaps (erased flash) with 0xFF. Raise RecordsBelowBaseError if
    any record sits below the app base."""
    records_in_range(hex_path)
    ih = IntelHex(hex_path)
    ih.padding = 0xFF
    return ih.tobinstr(start=APP_BASE, end=ih.maxaddr())


def _assemble_dat(
    *,
    app_size,
    image_hash,
    hw_version=HW_VERSION,
    fw_version=FW_VERSION,
    sd_req=SD_REQ,
    fw_type=dfu_cc_pb2.APPLICATION,
    signature=None,
):
    """Assemble a signed dfu-cc init packet (.dat) from primitive values and
    return the serialized bytes.

    Field presence and order replicate the stock golden exactly, so that with
    the golden's own app_size/hash/signature this reproduces the golden bytes.
    """
    packet = dfu_cc_pb2.Packet()
    signed = packet.signed_command
    signed.signature_type = dfu_cc_pb2.ECDSA_P256_SHA256
    signed.signature = PLACEHOLDER_SIGNATURE if signature is None else signature

    command = signed.command
    command.op_code = dfu_cc_pb2.INIT

    init = command.init
    init.fw_version = fw_version
    init.hw_version = hw_version
    init.sd_req.extend(sd_req)
    init.type = fw_type
    init.sd_size = 0
    init.bl_size = 0
    init.app_size = app_size
    init.hash.hash_type = dfu_cc_pb2.SHA256
    init.hash.hash = image_hash
    init.is_debug = False
    boot = init.boot_validation.add()
    boot.type = dfu_cc_pb2.VALIDATE_GENERATED_CRC
    boot.bytes = b""

    return packet.SerializeToString()


def build_init_packet(app_bin, *, hw_version=HW_VERSION, signature=None):
    """Return the .dat init-packet bytes describing app_bin (SHA256 embedded in
    Nordic byte-reversed order, app_size = len(app_bin))."""
    return _assemble_dat(
        app_size=len(app_bin),
        image_hash=_image_hash(app_bin),
        hw_version=hw_version,
        signature=signature,
    )


def build_manifest(bin_name, dat_name):
    """Return the manifest.json text for an application-only package, matching
    the stock 2v27 manifest schema."""
    manifest = {
        "manifest": {
            "application": {
                "bin_file": bin_name,
                "dat_file": dat_name,
            }
        }
    }
    return json.dumps(manifest, indent=4)


def decode_init_packet(dat_bytes):
    """Return the InitCommand from a Nordic DFU .dat.

    The packet is signed, so the InitCommand is nested under signed_command; an
    unsigned package would carry it under command.
    """
    packet = dfu_cc_pb2.Packet()
    packet.ParseFromString(dat_bytes)
    if packet.HasField("signed_command"):
        return packet.signed_command.command.init
    return packet.command.init


def extract_fields(init):
    """Return the enumerated, golden-comparable fields of an InitCommand."""
    return {
        "type": dfu_cc_pb2.FwType.Name(init.type),
        "sd_req": frozenset(init.sd_req),
        "hw_version": init.hw_version,
        "sd_size": init.sd_size,
        "bl_size": init.bl_size,
    }


def assert_fields_equal(generated_init, golden_init):
    """Raise FieldMismatchError if the two init packets differ on any enumerated
    field (type, sd_req, hw_version, sd_size, bl_size)."""
    generated = extract_fields(generated_init)
    golden = extract_fields(golden_init)
    mismatches = {
        name: (generated[name], golden[name])
        for name in REQUIRED_FIELDS
        if generated[name] != golden[name]
    }
    if mismatches:
        raise FieldMismatchError(mismatches)


def verify_image_hash(dat_bytes, app_bin):
    """Raise HashMismatchError if the init packet's embedded hash does not
    describe app_bin. Proves the .dat and the .bin in a package agree."""
    embedded = decode_init_packet(dat_bytes).hash.hash
    computed = _image_hash(app_bin)
    if embedded != computed:
        raise HashMismatchError(embedded, computed)


def build_package(hex_path, out_zip, golden_dat=DEFAULT_GOLDEN_DAT):
    """Build an app-only DFU .zip from hex_path and verify it against the golden.

    Return the generated package's enumerated fields (dict). Raise DfuPackageError
    on any check failure.
    """
    app_bin = extract_app_bin(hex_path)
    dat_bytes = build_init_packet(app_bin)
    manifest_text = build_manifest(BIN_NAME, DAT_NAME)

    golden_init = decode_init_packet(Path(golden_dat).read_bytes())
    generated_init = decode_init_packet(dat_bytes)
    # The package must match the golden's enumerated fields and its own image.
    assert_fields_equal(generated_init, golden_init)
    verify_image_hash(dat_bytes, app_bin)

    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(BIN_NAME, app_bin)
        z.writestr(DAT_NAME, dat_bytes)
        z.writestr(MANIFEST_NAME, manifest_text)

    return extract_fields(generated_init)


def _verify_dat(generated_dat_path, golden_dat_path):
    generated_init = decode_init_packet(Path(generated_dat_path).read_bytes())
    golden_init = decode_init_packet(Path(golden_dat_path).read_bytes())
    assert_fields_equal(generated_init, golden_init)
    print(
        f"OK: {generated_dat_path} init packet matches the golden field shape "
        f"{dict(sorted(extract_fields(generated_init).items()))}"
    )


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if argv and argv[0] == "--verify-dat":
            generated_dat = argv[1]
            golden_dat = argv[2] if len(argv) > 2 else DEFAULT_GOLDEN_DAT
            _verify_dat(generated_dat, golden_dat)
            return 0

        hex_path = argv[0] if len(argv) > 0 else "build/pebbleos.hex"
        out_zip = argv[1] if len(argv) > 1 else "pebbleos_banglejs2_app_dfu.zip"
        golden_dat = argv[2] if len(argv) > 2 else DEFAULT_GOLDEN_DAT
        fields = build_package(hex_path, out_zip, golden_dat=golden_dat)
        print(
            f"OK: {out_zip} is app-only; init packet matches the golden field "
            f"shape {dict(sorted(fields.items()))}"
        )
        return 0
    except DfuPackageError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
