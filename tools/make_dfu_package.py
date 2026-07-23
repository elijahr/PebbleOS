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

Note: if tools/dfu/dfu_cc_pb2.py is ever regenerated with protoc, re-add its
SPDX header (protoc drops it) AND run `ruff format` on it (protoc output is not
ruff-format-clean), or the CI compliance workflow will fail.

Structural fidelity is proven, not assumed: fed the stock golden's own
image-specific values, this tool's protobuf assembly reproduces the 146-byte
golden init packet (tools/dfu/golden/espruino_2v27_banglejs2_app.dat)
byte-for-byte (see tools/tests/test_make_dfu_package.py::test_assembly_
reproduces_golden_bytes). Every enumerated field is additionally checked
against the golden before the package is written, so a mis-built package
(wrong hardware, wrong SoftDevice requirement, wrong image type) is rejected
here rather than by the device.

Init-packet signature: the packet is signed the way nrfutil signs it -- ECDSA
over NIST P-256 with SHA-256 over the serialized InitCommand, r||s stored with
each 32-byte half byte-reversed (see the signing section below for the exact
scheme and source references). The Espruino bootloader has signature
verification disabled, so it requires a structurally valid signature of this
length but does not check it against any public key; the signing key here is a
public, security-irrelevant throwaway. The first hardware flash confirmed that
an all-zero placeholder signature is REJECTED at init-packet validation, so a
real, structurally valid signature is required.

Safety: a structurally wrong or hash-mismatched package is REJECTED by the
bootloader (and by this tool's own checks); it cannot brick a device via this
path (this rests on the stock bootloader's own reject-and-recover behavior).

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

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)
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

# 64-byte ECDSA P-256 signature length, matched to the golden.
SIGNATURE_LEN = 64

# --- Init-packet signing (Nordic Secure DFU / nrfutil scheme) ---------------
# nrfutil signs the init packet as follows (pc-nrfutil 6.1.7):
#   * The signed data is the serialized *InitCommand* -- the `init` sub-message,
#     NOT the enclosing Command. See init_packet_pb.get_init_command_bytes()
#     -> `return self.init_command.SerializeToString()`, invoked at
#     package.py:482 `signer.sign(init_packet.get_init_command_bytes())`.
#   * ECDSA over NIST P-256 (secp256r1), SHA-256 (signing.py sign()).
#   * The 64-byte signature is r||s, each 32-byte half byte-reversed to
#     little-endian: signing.py `return signature[31::-1] + signature[63:31:-1]`
#     over ecdsa's big-endian sigencode_string output.
#
# The Espruino bootloader requires a structurally valid signature of this length
# but does not verify it against any key (the stock 2v27 golden is signed with a
# key that is NOT nrfutil's default, and the first hardware flash rejected an
# all-zero signature). The key below is therefore a public, security-irrelevant
# throwaway. It is derived from a fixed scalar (no vendored PEM secret) and used
# with RFC 6979 deterministic signing, so the produced package is byte-stable.
_P256_ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
_THROWAWAY_SCALAR = (
    int.from_bytes(
        hashlib.sha256(b"pebbleos-banglejs2-dfu-throwaway-signing-key").digest(),
        "big",
    )
    % (_P256_ORDER - 1)
    + 1
)


def _throwaway_private_key():
    """Return the fixed, security-irrelevant P-256 signing key."""
    return ec.derive_private_key(_THROWAWAY_SCALAR, ec.SECP256R1())


def _nordic_encode_signature(r, s):
    """Encode ECDSA (r, s) the way nrfutil does: r||s, each 32 bytes little-endian
    (nrfutil signing.py: `signature[31::-1] + signature[63:31:-1]` over the
    big-endian r||s produced by sigencode_string)."""
    return r.to_bytes(32, "little") + s.to_bytes(32, "little")


def _nordic_decode_signature(signature):
    """Inverse of _nordic_encode_signature: recover (r, s) from Nordic's 64-byte
    little-endian r||s layout."""
    return int.from_bytes(signature[:32], "little"), int.from_bytes(
        signature[32:], "little"
    )


def sign_init_command(init_command_bytes, private_key=None):
    """Return the 64-byte Nordic-format ECDSA-P256-SHA256 signature over the
    serialized InitCommand bytes (the `init` sub-message), matching nrfutil."""
    key = _throwaway_private_key() if private_key is None else private_key
    der = key.sign(
        init_command_bytes, ec.ECDSA(hashes.SHA256(), deterministic_signing=True)
    )
    r, s = decode_dss_signature(der)
    return _nordic_encode_signature(r, s)


def verify_init_signature(init_command_bytes, signature, public_key=None):
    """Raise InvalidSignature if `signature` (Nordic 64-byte layout) is not a
    valid ECDSA-P256-SHA256 signature over init_command_bytes. Proves a generated
    signature is a structurally real ECDSA signature, not zeros or garbage."""
    key = _throwaway_private_key().public_key() if public_key is None else public_key
    r, s = _nordic_decode_signature(signature)
    key.verify(
        encode_dss_signature(r, s), init_command_bytes, ec.ECDSA(hashes.SHA256())
    )


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


class SignatureInvalidError(DfuPackageError):
    """The init packet's signature is not a valid ECDSA-P256-SHA256 signature
    over its own InitCommand (round-trip check failed)."""


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

    # nrfutil signs the serialized InitCommand (the `init` sub-message). When a
    # signature is supplied (e.g. the golden's, for byte-for-byte reproduction)
    # it is used verbatim; otherwise sign with the throwaway key.
    if signature is None:
        signature = sign_init_command(init.SerializeToString())
    signed.signature = signature

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


def verify_own_signature(dat_bytes):
    """Raise SignatureInvalidError unless the init packet carries a structurally
    real ECDSA-P256-SHA256 signature (throwaway key) over its own InitCommand.
    Round-trips the signature the tool just produced, proving it is not zeros or
    garbage."""
    packet = dfu_cc_pb2.Packet()
    packet.ParseFromString(dat_bytes)
    signed = packet.signed_command
    signature = signed.signature
    if len(signature) != SIGNATURE_LEN or signature == b"\x00" * SIGNATURE_LEN:
        raise SignatureInvalidError(
            "init-packet signature is missing, wrong length, or all zero"
        )
    try:
        verify_init_signature(signed.command.init.SerializeToString(), signature)
    except InvalidSignature as exc:
        raise SignatureInvalidError(
            "init-packet signature failed ECDSA-P256-SHA256 round-trip verification"
        ) from exc


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
    # The package must match the golden's enumerated fields and its own image,
    # and carry a structurally real signature (the bootloader rejects zeros).
    assert_fields_equal(generated_init, golden_init)
    verify_image_hash(dat_bytes, app_bin)
    verify_own_signature(dat_bytes)

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
