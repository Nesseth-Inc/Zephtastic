"""Synthetic full-ROM verification, public peer changes, and fail-closed diagnostics."""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from xiao_recovery_verify import verify
from xiao_release import BOARD, FLASH_SIZE, MAGIC, PRIMARY, SLOT
from xiao_restore_comparison import SETTINGS, Persisted, compare_images, compare_receipts

def sha(data):
    return hashlib.sha256(data).hexdigest()

OWNER = bytes(range(32))
NODE = zlib.crc32(OWNER)
PINS = [(2, bytes(range(1, 33))), (0xfffffffd, bytes(range(2, 34)))]

def crc_record(record):
    return record + struct.pack("<I", zlib.crc32(record))

def public_settings(through=0, pins=PINS):
    image = bytearray(b"\xff" * (FLASH_SIZE - SETTINGS))
    contents = [b"mtpki/nonces0\0mtpki/nonces1\0mtpki/contacts0\0mtpki/contacts1\0"]
    for generation in (1, through // 256 + 1):
        floor = (generation - 1) * 256
        contents.append(crc_record(struct.pack("<4sII32sIIII", b"ZTNC", 1, NODE, OWNER, 11, 12, floor, generation)))
    for gen in (5, 6):
        payload = struct.pack("<4sHHII32sIII", b"PCS1", 1, len(pins), gen, NODE, OWNER, 9, 11, 12)
        for peer, key in pins:
            payload += struct.pack("<I32s", peer, key)
        contents.append(crc_record(payload.ljust(636, b"\0")))
    raw = b"\xff".join(contents)
    image[:len(raw)] = raw
    return image

class Comparison(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        self.original = self.release(False)
        self.expanded = self.release(True)

    def release(self, expanded):
        target = "mt-zephyr-xiao-198" if expanded else "mt-zephyr-xiao"
        base = target + "-2.8.1+42"
        public = bytearray(b"\xff" * FLASH_SIZE)
        public[0] = 0xe0 + int(expanded)
        wire = (b"signed fixture application" + bytes([expanded])) * 4
        public[PRIMARY:PRIMARY + len(wire)] = wire
        public[PRIMARY + SLOT - 24] = 1
        public[PRIMARY + SLOT - 16:PRIMARY + SLOT] = MAGIC
        name = base + ".factory.bin"
        (self.work / name).write_bytes(public)
        manifest = {"version": "2.8.1+42", "board": BOARD, "pio_env": target,
                    "private_contact_capacity": 198 if expanded else 16,
                    "key_spki_sha256": sha(bytes([expanded])), "files": {
                    name: {"purpose": "fresh_install", "bytes": FLASH_SIZE, "sha256": sha(public)},
                    "firmware-" + base + "-update.bin": {"purpose": "wireless", "bytes": len(wire), "sha256": sha(wire)}}}
        if expanded:
            manifest.update(downgrade_requires_verified_recovery=True, ordinary_signer_sha256=sha(b"\x00"))
        path = self.work / (target + ".json")
        path.write_text(json.dumps(manifest))
        return (path, public)

    def capture(self, release, expanded, settings, *, change_offset=None, first=None, second=None):
        path, factory = release
        rom = factory.copy()
        rom[SETTINGS:] = settings
        if change_offset is not None:
            rom[change_offset] ^= 1
        digest = sha(rom)
        state = Persisted()
        confirmed = verify(io.BytesIO(rom), path, sha(path.read_bytes()), first or digest, second or digest,
                           expanded=expanded, observe=state.observe)
        return state, confirmed

    def evaluate(self, *, current_floor=0, current_pins=PINS, claimed_pins=None, observed_floor=None, change_offset=None,
                 alter=None, keys=PINS):
        a, old = self.capture(self.original, False, public_settings(pins=keys))
        b, new = self.capture(self.expanded, True, public_settings(current_floor, current_pins), change_offset=change_offset)
        def status(checksum, through, pins):
            return {"full_read_sha256": checksum, "physical_target_sha256": sha(b"test target"), "node": NODE,
                    "profile_owner_sha256": sha(OWNER), "nonce_reserved_through": through, "quiescent_acquisition": True,
                    "authoritative_pin_membership": True, "pinned_count": len(pins),
                    "authorized_pins": [{"node": n, "public_sha256": sha(k)} for n, k in pins]}
        saved = status(old["sha256"], 0, keys)
        current = status(new["sha256"], current_floor if observed_floor is None else observed_floor,
                         current_pins if claimed_pins is None else claimed_pins)
        if alter:
            alter(saved, current, new)
        return compare_images(a, b, old, new, saved, current)

    def test_equivalence_is_informational_only(self):
        result = self.evaluate()
        self.assertEqual(result["status"], "equivalent_persisted_state")
        self.assertFalse(result["same_identity_restore_authorized"])
        self.assertEqual(result["compared_persisted_bytes"], FLASH_SIZE - PRIMARY - SLOT)

    def test_advanced_floor_and_falsely_matching_report(self):
        for claimed, reason in ((256, "nonce_reservation_changed"), (0, "observed_floor_conflicts")):
            with self.subTest(claimed=claimed), self.assertRaisesRegex(ValueError, reason):
                self.evaluate(current_floor=256, observed_floor=claimed)

    def test_replay_storage_otherwise_invisible_to_public_comparison(self):
        for where in (0x390000, 0x3e0000, 0x480000, 0x7ef000, 0x7fffff):
            with self.subTest(offset=where), self.assertRaisesRegex(ValueError, "persistent_state_differs"):
                self.evaluate(change_offset=where)

    def test_revoke_rotate_add_and_forged_membership(self):
        for items in (PINS[:1], [(2, b"A" * 32), PINS[1]]):
            with self.subTest(pins=items), self.assertRaisesRegex(ValueError, "revoked_or_rotated"):
                self.evaluate(current_pins=items)
        with self.assertRaisesRegex(ValueError, "membership_changed"):
            self.evaluate(current_pins=[PINS[0], (3, b"B" * 32), PINS[1]])
        def forge(saved, current, new):
            for obs in (saved, current):
                obs.update(pinned_count=0, authorized_pins=[])
        with self.assertRaisesRegex(ValueError, "unconfirmed_pin_membership"):
            self.evaluate(alter=forge)

    def test_identity_authority_source_and_duplicates(self):
        for fn, reason in (
            (lambda a, b, r: b.update(node=3), "owner_identity"),
            (lambda a, b, r: b.update(profile_owner_sha256=sha(b"wrong")), "owner_identity"),
            (lambda a, b, r: r.update(required_ordinary_signer=sha(b"other")), "authority_pair"),
            (lambda a, b, r: b.update(physical_target_sha256=sha(b"device")), "target_changed"),
            (lambda a, b, r: b.update(full_read_sha256=sha(b"old")), "unbound_owner"),
            (lambda a, b, r: b.update(authoritative_pin_membership=False), "unbound_owner"),
            (lambda a, b, r: b.update(quiescent_acquisition=False), "unbound_owner"),
            (lambda a, b, r: b.update(pinned_count=2, authorized_pins=b["authorized_pins"][:1]*2), "ambiguous_pin"),
        ):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                self.evaluate(alter=fn)

    def test_cli_uses_separate_verified_streams_and_never_exits_as_restore_permission(self):
        command = [sys.executable, "-B", str(Path(__file__).resolve().parents[2] / "scripts" / "xiao_recovery_verify.py"),
                   "pre-restore"]
        streams = []
        for flag, release, name in (("ordinary", self.original, "saved"), ("expanded", self.expanded, "current")):
            manifest, factory = release
            rom = factory.copy()
            rom[SETTINGS:] = public_settings()
            data = self.work / (flag + ".synthetic")
            data.write_bytes(rom)
            digest = sha(rom)
            state = self.work / (flag + "-state.json")
            state.write_text(json.dumps({"full_read_sha256": digest, "physical_target_sha256": sha(b"test device"),
                "node": NODE, "profile_owner_sha256": sha(OWNER), "nonce_reserved_through": 0,
                "quiescent_acquisition": True, "authoritative_pin_membership": True, "pinned_count": len(PINS),
                "authorized_pins": [{"node": n, "public_sha256": sha(k)} for n, k in PINS]}))
            command += ["--" + name + "-state", state, "--" + flag + "-manifest", manifest,
                        "--" + flag + "-manifest-sha256", sha(manifest.read_bytes()),
                        "--" + flag + "-first-rom-sha256", digest, "--" + flag + "-second-rom-sha256", digest]
            streams.append(data)
        for corrupt_pair in (False, True):
            with streams[0].open("rb") as saved, streams[1].open("rb") as current:
                attempt = list(command)
                if corrupt_pair:
                    attempt[attempt.index("--expanded-second-rom-sha256") + 1] = sha(b"different independent read")
                p = subprocess.run(attempt + ["--current-fd", str(current.fileno())], pass_fds=(current.fileno(),),
                                   stdin=saved, capture_output=True, check=False)
            self.assertEqual(p.returncode, 2 if corrupt_pair else 3)
            row = json.loads(p.stdout)
            self.assertEqual(row["status"], "refused" if corrupt_pair else "equivalent_persisted_state")
            self.assertFalse(row["same_identity_restore_authorized"])

    def test_independent_current_rom_reads_are_required(self):
        with self.assertRaisesRegex(ValueError, "device reads disagree"):
            self.capture(self.expanded, True, public_settings(), second=sha(b"truncated"))

    def test_legacy_receipts_never_admit(self):
        digest=sha(b"original");old={"preconversion_sha256":digest,"preconversion_bytes":FLASH_SIZE,
            "source_owned_recovery":{"sha256":digest,"verified_bytes":FLASH_SIZE},
            "independent_rom_read_digests":{"first":digest,"second":digest,"bytes_per_read":FLASH_SIZE},
            "verified_signed_primary_and_trailer":True,"record_crc_and_live_public_fingerprint_match":True,
            "nonce_reserved_through":0}
        live={"verified_disposable_key":True,"profile_owner_sha256":sha(OWNER),"node":NODE,"tx_off":True,
              "rf_off":True,"pio_env":"mt-zephyr-xiao-198"}
        for through in (0,256):
            result=compare_receipts(old,dict(live,nonce_reserved_through=through))
            self.assertEqual(result["status"],"refused")
            self.assertFalse(result["same_identity_restore_authorized"])
if __name__ == "__main__":
    unittest.main()
