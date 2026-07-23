#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

"""Host test for tools/make_dfu_package.py (pure-Python, hand-rolled packager).

The tool builds a Nordic Secure DFU application-only package with no nrfutil:
it extracts the contiguous app image from build/pebbleos.hex at the DFU app
base, hand-rolls the dfu-cc init packet (structurally matched to the stock
Espruino 2v27 golden), writes the manifest, and zips the three files.

These tests prove the construction is faithful and the verification BITES:

  - Serialization fidelity: fed the golden's own image-specific values, the
    tool's protobuf assembly reproduces the 146-byte golden .dat BYTE-FOR-BYTE.
    This pins every structural field (op_code, fw/hw version, sd_req order,
    type, sd_size, bl_size, is_debug, boot_validation, signature framing).
  - Field contract: a generated init packet's enumerated fields equal the
    golden's (type, sd_req, hw_version, sd_size, bl_size).
  - Image self-consistency: the embedded hash is the Nordic-convention
    (byte-reversed) SHA256 of the exact image the package carries.
  - The records-in-range check rejects any hex record below the app base.
  - The field check rejects a wrong hardware version and a wrong sd_req set.
  - The image-hash check rejects a package whose .dat describes a different
    image than its .bin.

The unit tests need only protobuf + the vendored golden .dat. The end-to-end
test additionally needs a relinked build/pebbleos.hex; it skips otherwise.
"""

import hashlib
import json
import os
import sys
import tempfile
import unittest
import zipfile

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from intelhex import IntelHex

# Allow running from anywhere (mirrors tools/tests/test_hdlc.py).
root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
sys.path.insert(0, root_dir)

import make_dfu_package as mdp  # noqa: E402  (needs tools/ on sys.path)

REPO_ROOT = os.path.abspath(os.path.join(root_dir, os.pardir))
GOLDEN_DAT = os.path.join(
    REPO_ROOT, "tools", "dfu", "golden", "espruino_2v27_banglejs2_app.dat"
)
RELINKED_HEX = os.path.join(REPO_ROOT, "build", "pebbleos.hex")

# The stock 2v27 manifest schema, transcribed from the golden package
# (espruino_2v27_banglejs2.zip -> manifest.json). Only the file names differ
# between the stock package and ours; the key structure is identical.
GOLDEN_MANIFEST = {
    "manifest": {
        "application": {
            "bin_file": "espruino_2v27_banglejs2_app.bin",
            "dat_file": "espruino_2v27_banglejs2_app.dat",
        }
    }
}

# A deterministic, non-trivial stand-in application image. Real bytes so the
# SHA256 is meaningful; small so tests stay fast.
SAMPLE_APP_BIN = bytes(range(256)) * 4  # 1024 bytes, every byte value present
OTHER_APP_BIN = SAMPLE_APP_BIN + b"\xa5"  # a genuinely different image


def _read_golden_bytes():
    with open(GOLDEN_DAT, "rb") as f:
        return f.read()


def _golden_init():
    return mdp.decode_init_packet(_read_golden_bytes())


def _golden_signature():
    p = mdp.dfu_cc_pb2.Packet()
    p.ParseFromString(_read_golden_bytes())
    return p.signed_command.signature


def _write_hex(min_addr, extra_addrs=()):
    """Write a tiny Intel HEX with one byte at each given address; return path."""
    ih = IntelHex()
    ih[min_addr] = 0xAA
    for a in extra_addrs:
        ih[a] = 0xBB
    fd, path = tempfile.mkstemp(suffix=".hex")
    os.close(fd)
    ih.write_hex_file(path)
    return path


class TestExtractAppBin(unittest.TestCase):
    def test_extract_returns_contiguous_padded_image_from_base(self):
        # Bytes at base, base+1, base+3 -> gap at base+2 pads with 0xFF (erased
        # flash), and the image starts exactly at the app base.
        ih = IntelHex()
        ih[mdp.APP_BASE] = 0xAA
        ih[mdp.APP_BASE + 1] = 0xBB
        ih[mdp.APP_BASE + 3] = 0xCC
        fd, path = tempfile.mkstemp(suffix=".hex")
        os.close(fd)
        ih.write_hex_file(path)
        try:
            self.assertEqual(mdp.extract_app_bin(path), b"\xaa\xbb\xff\xcc")
        finally:
            os.unlink(path)

    def test_record_below_base_is_rejected(self):
        # A record at 0x8000 (a pre-relink app base) must be caught.
        bad = _write_hex(0x8000, extra_addrs=(mdp.APP_BASE,))
        try:
            with self.assertRaises(mdp.RecordsBelowBaseError) as cm:
                mdp.extract_app_bin(bad)
            self.assertEqual(cm.exception.min_addr, 0x8000)
            self.assertEqual(cm.exception.app_base, 0x26000)
        finally:
            os.unlink(bad)

    @unittest.skipUnless(os.path.exists(RELINKED_HEX), "no build/pebbleos.hex")
    def test_relinked_firmware_hex_starts_at_app_base(self):
        self.assertEqual(mdp.records_in_range(RELINKED_HEX), 0x26000)


class TestSerializationFidelity(unittest.TestCase):
    def test_assembly_reproduces_golden_bytes(self):
        # Fed the golden's own image-specific values (app_size, hash) and its
        # real signature, the tool's protobuf assembly must reproduce the golden
        # .dat byte-for-byte. Everything else (versions, sd_req order, type,
        # sizes, is_debug, boot_validation, op_code, signature framing) comes
        # from the tool's own defaults, so a byte match pins all of it.
        golden = _read_golden_bytes()
        g_init = _golden_init()
        rebuilt = mdp._assemble_dat(
            app_size=g_init.app_size,
            image_hash=g_init.hash.hash,
            signature=_golden_signature(),
        )
        self.assertEqual(rebuilt, golden)

    def test_generated_packet_equals_golden_except_image_fields(self):
        # Full protobuf-message equality: build a real init packet, inject the
        # golden signature to isolate the two legitimately image-specific fields
        # (app_size, hash), then assert the generated Packet equals the golden
        # Packet with only those two fields substituted.
        gen_dat = mdp.build_init_packet(SAMPLE_APP_BIN, signature=_golden_signature())
        gen_pkt = mdp.dfu_cc_pb2.Packet()
        gen_pkt.ParseFromString(gen_dat)

        expected = mdp.dfu_cc_pb2.Packet()
        expected.ParseFromString(_read_golden_bytes())
        expected.signed_command.command.init.app_size = len(SAMPLE_APP_BIN)
        expected.signed_command.command.init.hash.hash = hashlib.sha256(
            SAMPLE_APP_BIN
        ).digest()[::-1]
        self.assertEqual(gen_pkt, expected)


class TestFieldContract(unittest.TestCase):
    def test_golden_decodes_to_exact_enumerated_fields(self):
        self.assertEqual(
            mdp.extract_fields(_golden_init()),
            {
                "type": "APPLICATION",
                "sd_req": frozenset({0xA9, 0xAE, 0xB6}),
                "hw_version": 52,
                "sd_size": 0,
                "bl_size": 0,
            },
        )

    def test_generated_enumerated_fields_equal_golden(self):
        gen_init = mdp.decode_init_packet(mdp.build_init_packet(SAMPLE_APP_BIN))
        self.assertEqual(
            mdp.extract_fields(gen_init), mdp.extract_fields(_golden_init())
        )
        self.assertEqual(mdp.extract_fields(gen_init), mdp.REQUIRED_FIELDS)

    def test_wrong_hw_version_is_rejected_by_field_check(self):
        # The SAME builder with a wrong hardware version produces a package the
        # field check rejects -- proof the check is load-bearing.
        wrong = mdp.decode_init_packet(
            mdp.build_init_packet(SAMPLE_APP_BIN, hw_version=51)
        )
        with self.assertRaises(mdp.FieldMismatchError) as cm:
            mdp.assert_fields_equal(wrong, _golden_init())
        self.assertEqual(cm.exception.mismatches, {"hw_version": (51, 52)})

    def test_wrong_sd_req_is_rejected_by_field_check(self):
        # A packet whose SoftDevice-requirement set drops the golden's 0xB6
        # FWID is rejected by the field check -- proof the sd_req set comparison
        # in assert_fields_equal bites, not just the scalar fields.
        wrong = mdp.decode_init_packet(
            mdp._assemble_dat(
                app_size=len(SAMPLE_APP_BIN),
                image_hash=mdp._image_hash(SAMPLE_APP_BIN),
                sd_req=(0xA9, 0xAE),
            )
        )
        with self.assertRaises(mdp.FieldMismatchError) as cm:
            mdp.assert_fields_equal(wrong, _golden_init())
        self.assertEqual(
            cm.exception.mismatches,
            {"sd_req": (frozenset({0xA9, 0xAE}), frozenset({0xA9, 0xAE, 0xB6}))},
        )


class TestImageSelfConsistency(unittest.TestCase):
    def test_embedded_hash_is_reversed_sha256_and_app_size_matches(self):
        init = mdp.decode_init_packet(mdp.build_init_packet(SAMPLE_APP_BIN))
        self.assertEqual(init.app_size, len(SAMPLE_APP_BIN))
        self.assertEqual(init.hash.hash_type, mdp.dfu_cc_pb2.SHA256)
        # Nordic stores the digest in little-endian (byte-reversed) order.
        self.assertEqual(init.hash.hash, hashlib.sha256(SAMPLE_APP_BIN).digest()[::-1])

    def test_verify_image_hash_accepts_matching_image(self):
        dat = mdp.build_init_packet(SAMPLE_APP_BIN)
        # Returns None (no raise) when the .dat describes exactly this image.
        self.assertIsNone(mdp.verify_image_hash(dat, SAMPLE_APP_BIN))

    def test_verify_image_hash_rejects_different_image(self):
        # A .dat that describes SAMPLE_APP_BIN must be rejected against a
        # different image -- proof the hash check bites.
        dat = mdp.build_init_packet(SAMPLE_APP_BIN)
        with self.assertRaises(mdp.HashMismatchError) as cm:
            mdp.verify_image_hash(dat, OTHER_APP_BIN)
        self.assertEqual(
            cm.exception.embedded, hashlib.sha256(SAMPLE_APP_BIN).digest()[::-1]
        )
        self.assertEqual(
            cm.exception.computed, hashlib.sha256(OTHER_APP_BIN).digest()[::-1]
        )


class TestSignature(unittest.TestCase):
    """The init packet must carry a structurally real ECDSA-P256-SHA256
    signature (Nordic/nrfutil scheme), because the Espruino bootloader rejects
    an all-zero placeholder at init-packet validation."""

    def _default_signed(self):
        pkt = mdp.dfu_cc_pb2.Packet()
        pkt.ParseFromString(mdp.build_init_packet(SAMPLE_APP_BIN))
        return pkt.signed_command

    def test_default_signature_is_real_64_byte_nonzero(self):
        signed = self._default_signed()
        self.assertEqual(signed.signature_type, mdp.dfu_cc_pb2.ECDSA_P256_SHA256)
        self.assertEqual(len(signed.signature), 64)
        self.assertNotEqual(signed.signature, b"\x00" * 64)

    def test_signature_roundtrip_verifies_over_init_command(self):
        # The signature is a valid ECDSA-P256-SHA256 signature over the serialized
        # InitCommand (nrfutil signs the `init` sub-message, not the Command).
        signed = self._default_signed()
        # Returns None (no raise) when the signature is structurally valid.
        self.assertIsNone(
            mdp.verify_init_signature(
                signed.command.init.SerializeToString(), signed.signature
            )
        )

    def test_signature_is_over_init_command_not_command(self):
        # Fidelity to nrfutil: verifying over the *Command* bytes must FAIL,
        # proving the tool signs the InitCommand and not the Command wrapper.
        signed = self._default_signed()
        with self.assertRaises(InvalidSignature):
            mdp.verify_init_signature(
                signed.command.SerializeToString(), signed.signature
            )

    def test_signature_byte_order_is_nordic_little_endian(self):
        # The 64-byte layout is r||s each byte-reversed to little-endian. Decoding
        # it as big-endian (r||s straight) must FAIL to verify, proving the
        # nrfutil reversal (signature[31::-1] + signature[63:31:-1]) is applied.
        signed = self._default_signed()
        init_bytes = signed.command.init.SerializeToString()
        sig = signed.signature
        r_be = int.from_bytes(sig[:32], "big")
        s_be = int.from_bytes(sig[32:], "big")
        pub = mdp._throwaway_private_key().public_key()
        with self.assertRaises(InvalidSignature):
            pub.verify(
                encode_dss_signature(r_be, s_be),
                init_bytes,
                ec.ECDSA(hashes.SHA256()),
            )

    def test_verify_own_signature_rejects_zeroed_signature(self):
        # verify_own_signature bites: a package whose signature is zeroed out is
        # rejected (this is exactly what the bootloader rejected on real hardware).
        pkt = mdp.dfu_cc_pb2.Packet()
        pkt.ParseFromString(mdp.build_init_packet(SAMPLE_APP_BIN))
        pkt.signed_command.signature = b"\x00" * 64
        with self.assertRaises(mdp.SignatureInvalidError):
            mdp.verify_own_signature(pkt.SerializeToString())

    def test_verify_own_signature_accepts_real_package(self):
        self.assertIsNone(
            mdp.verify_own_signature(mdp.build_init_packet(SAMPLE_APP_BIN))
        )

    def test_signing_is_deterministic(self):
        # RFC 6979 deterministic signing + fixed throwaway key => byte-stable
        # output, so the package stays reproducible across rebuilds.
        self.assertEqual(
            mdp.build_init_packet(SAMPLE_APP_BIN),
            mdp.build_init_packet(SAMPLE_APP_BIN),
        )


class TestManifest(unittest.TestCase):
    def test_manifest_content_and_schema_match_golden(self):
        manifest_str = mdp.build_manifest(
            "pebbleos_banglejs2_app.bin", "pebbleos_banglejs2_app.dat"
        )
        self.assertEqual(
            json.loads(manifest_str),
            {
                "manifest": {
                    "application": {
                        "bin_file": "pebbleos_banglejs2_app.bin",
                        "dat_file": "pebbleos_banglejs2_app.dat",
                    }
                }
            },
        )

        # Same key structure as the stock golden manifest (only names differ).
        def shape(d):
            return {
                k: (shape(v) if isinstance(v, dict) else type(v).__name__)
                for k, v in sorted(d.items())
            }

        self.assertEqual(shape(json.loads(manifest_str)), shape(GOLDEN_MANIFEST))


class TestEndToEndPackaging(unittest.TestCase):
    @unittest.skipUnless(os.path.exists(RELINKED_HEX), "no build/pebbleos.hex")
    def test_build_package_from_relinked_hex(self):
        with tempfile.TemporaryDirectory() as td:
            out_zip = os.path.join(td, "app_dfu.zip")
            fields = mdp.build_package(RELINKED_HEX, out_zip, golden_dat=GOLDEN_DAT)
            # The tool returns the generated package's enumerated fields, which
            # equal the golden contract.
            self.assertEqual(fields, mdp.REQUIRED_FIELDS)
            self.assertEqual(fields, mdp.extract_fields(_golden_init()))

            with zipfile.ZipFile(out_zip) as z:
                names = sorted(z.namelist())
                zbin = z.read("pebbleos_banglejs2_app.bin")
                zdat = z.read("pebbleos_banglejs2_app.dat")
                zmanifest = z.read("manifest.json")

            # Application-only: exactly manifest + one .bin + one .dat.
            self.assertEqual(
                names,
                [
                    "manifest.json",
                    "pebbleos_banglejs2_app.bin",
                    "pebbleos_banglejs2_app.dat",
                ],
            )
            # The zipped image is exactly the extracted app region.
            self.assertEqual(zbin, mdp.extract_app_bin(RELINKED_HEX))
            # The zipped .dat describes the zipped .bin (reversed SHA256).
            init = mdp.decode_init_packet(zdat)
            self.assertEqual(init.hash.hash, hashlib.sha256(zbin).digest()[::-1])
            self.assertEqual(init.app_size, len(zbin))
            # Manifest names the two real members.
            self.assertEqual(
                json.loads(zmanifest),
                {
                    "manifest": {
                        "application": {
                            "bin_file": "pebbleos_banglejs2_app.bin",
                            "dat_file": "pebbleos_banglejs2_app.dat",
                        }
                    }
                },
            )


if __name__ == "__main__":
    unittest.main()
