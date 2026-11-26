"""Release packaging must preserve the signed wire bytes and the two different slot trailers."""
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import xiao_release as release
from xiao_recovery_verify import verify


def tlv(tag, value):
    return struct.pack("<HH", tag, len(value)) + value


class SigningBudget(unittest.TestCase):
    def test_largest_supported_signature_fits_at_the_unsigned_limit(self):
        # Two 33-byte positive integers, their DER headers, and the SEQUENCE.
        integer = bytes.fromhex("02210080") + bytes(31)
        signature = bytes.fromhex("3046") + integer * 2
        entries = tlv(1, bytes(32)) + tlv(0x10, bytes(32)) + tlv(0x22, signature)
        metadata = struct.pack("<HH", 0x6907, 4 + len(entries)) + entries
        raw_limit = release.unsigned_update_capacity(release.SLOT)
        self.assertEqual(raw_limit + len(metadata) + release.SWAP_TRAILER_RESERVE, release.SLOT)
        self.assertGreater(raw_limit + 1 + len(metadata), release.SLOT - release.SWAP_TRAILER_RESERVE)


class ExpandedBuild(unittest.TestCase):
    def test_refuse_missing_large_directory_or_first_use_support(self):
        base = {
            "BOOTLOADER_MCUBOOT", "MT_SIGNED_WIFI_OTA", "IMG_ERASE_PROGRESSIVELY",
            "MT_STORE_FORWARD_PSRAM", "MT_PKI_TARGET_RUNTIME", "MT_BROWSER_MESSAGING",
            "MT_SATELLITE_HISTORY", "MT_PAGED_SATELLITES", "MT_SIGNED_BLE_OTA",
            release.EXPANDED, "MT_XIAO_LARGE_NODEDB", "MT_SATELLITE_INITIALIZE_ERASED",
        }
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            app = build / "app/zephyr"
            app.mkdir(parents=True)
            for missing in ("MT_XIAO_LARGE_NODEDB", "MT_SATELLITE_INITIALIZE_ERASED"):
                with self.subTest(missing=missing):
                    options = [f"CONFIG_{item}=y" for item in sorted(base - {missing})]
                    (app / ".config").write_text("\n".join(options) + "\n")
                    with self.assertRaisesRegex(ValueError, f"requires {missing}"):
                        release.check_build(build, build / "unused.pem", expanded=True)


    def test_stock_identity_import_only_in_ordinary_release(self):
        common = {
            "BOOTLOADER_MCUBOOT", "MT_SIGNED_WIFI_OTA", "IMG_ERASE_PROGRESSIVELY",
            "MT_STORE_FORWARD_PSRAM", "MT_PKI_TARGET_RUNTIME", "MT_BROWSER_MESSAGING",
            "MT_SATELLITE_HISTORY", "MT_PAGED_SATELLITES", "MT_SIGNED_BLE_OTA",
        }
        other = {release.EXPANDED, "MT_XIAO_LARGE_NODEDB", "MT_SATELLITE_INITIALIZE_ERASED"}
        with tempfile.TemporaryDirectory() as path:
            build = Path(path)
            app = build / "app/zephyr"
            app.mkdir(parents=True)
            for expanded, extras in ((False, set()), (True, other | {"MT_STOCK_IDENTITY_IMPORT"})):
                with self.subTest(expanded=expanded):
                    (app / ".config").write_text("".join(f"CONFIG_{key}=y\n" for key in common | extras))
                    with self.assertRaisesRegex(ValueError, "MT_STOCK_IDENTITY_IMPORT"):
                        release.check_build(build, build / "unused.pem", expanded=expanded, newly_built=True)


class Packaging(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.pub = self.root / "trust.pem"
        self.pub.write_bytes(self.key.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        der = release.public_der(self.pub)
        self.loader = b"\xe9" + der
        self.key_hash = hashlib.sha256(der).digest()
        payload = struct.pack("<I", 0xace637d3) + der[-65:] + self.key_hash
        self.raw = bytes(32) + payload
        header = struct.pack("<IIHHIIBBHI4x", 0x96f3b83d, 0, 32, 0, len(payload), 0, 2, 8, 1, 42)
        self.body = header + payload
        self.sig = self.key.sign(self.body, ec.ECDSA(hashes.SHA256()))
        self.signed = self.root / "signed.bin"
        validate_build = patch.object(release, "check_build", return_value=(self.raw, self.loader))
        validate_build.start()
        self.addCleanup(validate_build.stop)

    def write_image(self, signature, suffix=b""):
        pairs = tlv(1, self.key_hash) + tlv(0x10, hashlib.sha256(self.body).digest()) + tlv(0x22, signature)
        self.signed.write_bytes(self.body + struct.pack("<HH", 0x6907, len(pairs) + 4) + pairs + suffix)

    def check_mobile_files(self, out, manifest):
        target = manifest["pio_env"] + "-" + manifest["version"]
        android = "firmware-" + target + "-update.bin"
        apple = "firmware-" + target + ".bin"
        expected = {
            android: "wireless",
            apple: "wireless_apple",
            target + "-usb-update.bin": "stock_usb_secondary",
            target + ".factory.bin": "fresh_install",
        }
        self.assertEqual({name: value["purpose"] for name, value in manifest["files"].items()}, expected)
        for name, item in manifest["files"].items():
            data = (out / name).read_bytes()
            self.assertEqual((len(data), release.sha(data)), (item["bytes"], item["sha256"]))
            if name in (android, apple):
                self.assertEqual(data, self.signed.read_bytes())
            else:
                # Both stock mobile pickers exclude mt-* files. Apple's local
                # parser accepts a .factory.bin with a firmware- prefix.
                self.assertTrue(name.startswith("mt-"))

    def test_public_files_match_their_intended_boot_state(self):
        self.write_image(self.sig)
        out = self.root / "release"
        release.make_package(self.root, self.pub, self.signed, out)
        manifest = json.loads((out / "manifest.json").read_bytes())
        files = {info["purpose"]: (out / name).read_bytes() for name, info in manifest["files"].items()}
        wire = self.signed.read_bytes()
        pending = wire.ljust(release.SLOT - 16, b"\xff") + release.MAGIC
        self.assertEqual(manifest["version"], "2.8.1+42")
        self.check_mobile_files(out, manifest)
        self.assertEqual(files["wireless"], wire)
        self.assertEqual(files["stock_usb_secondary"], pending)
        confirmed = bytearray(pending)
        confirmed[-24] = 1
        factory = bytearray(b"\xff") * release.FLASH_SIZE
        factory[:len(self.loader)] = self.loader
        factory[release.PRIMARY:release.PRIMARY + release.SLOT] = confirmed
        self.assertEqual(files["fresh_install"], factory)

    def test_expanded_profile_requires_a_separate_known_ordinary_signer(self):
        self.write_image(self.sig)
        ordinary = self.root / "ordinary.pem"
        ordinary.write_bytes(ec.generate_private_key(ec.SECP256R1()).public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        out = self.root / "expanded-release"

        def pack(comparison):
            return release.make_package(self.root, self.pub, self.signed, out,
                                        expanded=True, ordinary_public_key=comparison)

        for invalid in (None, self.pub):
            with self.subTest(comparison=invalid), self.assertRaises(ValueError):
                pack(invalid)
        receipt = self.root / "ordinary-signer.sha256"
        receipt.write_text("0" * 64)
        with self.assertRaisesRegex(ValueError, "checked at build time"):
            pack(ordinary)
        self.assertFalse(out.exists())
        ordinary_hash = release.sha(release.public_der(ordinary))
        receipt.write_text(ordinary_hash)
        manifest = pack(ordinary)
        self.assertEqual(manifest["pio_env"], "mt-zephyr-xiao-198")
        self.assertEqual(manifest["private_contact_capacity"], 198)
        self.assertEqual(manifest["ordinary_signer_sha256"], ordinary_hash)
        self.assertTrue(manifest["downgrade_requires_verified_recovery"])
        self.check_mobile_files(out, manifest)
        factory = next(name for name, info in manifest["files"].items() if info["purpose"] == "fresh_install")
        # First boot prepares LittleFS. USB activation separately owns the adjacent trust banks.
        self.assertEqual((out / factory).read_bytes()[0x400000:0x488000], b"\xff" * 0x88000)

    def test_recovery_requires_both_rom_reads_and_the_original_normal_install(self):
        self.write_image(self.sig)
        out = self.root / "ordinary"
        manifest = release.make_package(self.root, self.pub, self.signed, out)
        manifest_file = out / "manifest.json"
        manifest_sha = release.sha(manifest_file.read_bytes())
        name = next(k for k, v in manifest["files"].items() if v["purpose"] == "fresh_install")
        memory = bytearray((out / name).read_bytes())
        memory[0x7f0020:0x7f0024] = b"TEST"  # synthetic settings
        expected = release.sha(memory)

        def check(data, first=expected, second=expected):
            return verify(io.BytesIO(data), manifest_file, manifest_sha, first, second)

        self.assertEqual(check(memory)["verified_bytes"], release.FLASH_SIZE)
        with self.assertRaisesRegex(ValueError, "disagree"):
            check(memory, second="0" * 64)
        for offset in (0, release.PRIMARY + 40, release.PRIMARY - 1,
                       release.PRIMARY + release.SLOT - 24, 0x480000, 0x7f0020):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                broken = memory.copy()
                broken[offset] ^= 1
                check(broken)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            check(memory[:-1])

        # Previously published ordinary releases only had the Android wireless
        # name, and their factory reference began with firmware-.
        legacy = "firmware-mt-zephyr-xiao-2.8.1+42.factory.bin"
        (out / name).rename(out / legacy)
        manifest["files"][legacy] = manifest["files"].pop(name)
        manifest["files"].pop("firmware-mt-zephyr-xiao-2.8.1+42.bin")
        manifest_file.write_text(json.dumps(manifest))
        manifest_sha = release.sha(manifest_file.read_bytes())
        self.assertEqual(check(memory)["verified_bytes"], release.FLASH_SIZE)

        manifest["files"][name] = manifest["files"][legacy]
        manifest_file.write_text(json.dumps(manifest))
        manifest_sha = release.sha(manifest_file.read_bytes())
        with self.assertRaisesRegex(ValueError, "one ordinary factory reference"):
            check(memory)

    def test_rejects_inputs_the_wireless_receiver_cannot_accept(self):
        other = ec.generate_private_key(ec.SECP256R1())
        for sig, suffix in [(self.sig + b"\x00", b""), (other.sign(self.body, ec.ECDSA(hashes.SHA256())), b""),
                            (self.sig, b"\xff" * 16 + release.MAGIC)]:
            with self.subTest(signature_length=len(sig), suffix=bool(suffix)):
                self.write_image(sig, suffix)
                with self.assertRaises(ValueError):
                    release.make_package(self.root, self.pub, self.signed, self.root / "release")
                self.assertFalse((self.root / "release").exists())


if __name__ == "__main__":
    unittest.main()
