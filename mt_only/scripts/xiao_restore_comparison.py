"""Read-only same-owner comparison for signed XIAO recovery. No device or writer API."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import zlib

from xiao_release import FLASH_SIZE, PRIMARY, SLOT, require

DIGEST = re.compile("[0-9a-f]{64}")
SETTINGS = 0x7f0000


def fingerprint(value):
    return isinstance(value, str) and DIGEST.fullmatch(value) is not None


def reservation(value):
    return type(value) is int and 0 <= value <= 0xffffffff and (value == 0xffffffff or value % 256 == 0)


def json_object(path):
    def unique(pairs):
        obj = {}
        for k, v in pairs:
            require(k not in obj, "ambiguous_duplicate_json_field")
            obj[k] = v
        return obj
    require(path.stat().st_size <= 262144, "oversize_evidence")
    obj = json.loads(path.read_bytes(), object_pairs_hook=unique)
    require(isinstance(obj, dict), "invalid_evidence")
    return obj


def compare_receipts(saved, current):
    """Legacy observations can show a contradiction, never establish freshness."""
    archive = saved.get("source_owned_recovery", {})
    pair = saved.get("independent_rom_read_digests", {})
    digest = archive.get("sha256")
    require(fingerprint(digest) and digest == saved["preconversion_sha256"] == pair["first"] == pair["second"] and
            archive["verified_bytes"] == pair["bytes_per_read"] == saved["preconversion_bytes"] == FLASH_SIZE and
            saved["verified_signed_primary_and_trailer"] is True and saved["record_crc_and_live_public_fingerprint_match"] is True,
            "unverified_original_archive_receipt")
    require(current["verified_disposable_key"] is True and fingerprint(current["profile_owner_sha256"]) and
            type(current["node"]) is int and current["rf_off"] is True and current["tx_off"] is True and
            current["pio_env"] == "mt-zephyr-xiao-198", "unverified_current_observation")
    before, after = saved["nonce_reserved_through"], current["nonce_reserved_through"]
    require(reservation(before) and reservation(after), "invalid_nonce_floor")
    return {"status": "refused", "same_identity_restore_authorized": False, "archive_sha256": digest,
            "saved_reserved_through": before, "current_reserved_through": after,
            "reason": "observed_nonce_reservation_changed" if before != after else "receipts_cannot_establish_freshness"}


def pinset(state):
    count, rows = state["pinned_count"], state["authorized_pins"]
    require(type(count) is int and type(rows) is list and 0 <= count == len(rows) <= 198, "incomplete_pin_membership")
    out = {}
    keys = set()
    for row in rows:
        node, pub = row["node"], row["public_sha256"]
        require(type(node) is int and 2 <= node <= 0xfffffffd and node != state["node"] and fingerprint(pub) and
                node not in out and pub not in keys and pub != state["profile_owner_sha256"], "ambiguous_pin_membership")
        out[node] = pub
        keys.add(pub)
    return out


class Persisted:
    """Compare all bytes outside the loader and OTA slots, including unused regions and scratch."""
    def __init__(self):
        self.digest = hashlib.sha256()
        self.settings = bytearray()
        self.bytes = 0

    def observe(self, offset, block):
        end = offset + len(block)
        start = PRIMARY + SLOT
        if end > start:
            view = memoryview(block)[max(0, start - offset):]
            self.digest.update(view)
            self.bytes += len(view)
        if end > SETTINGS:
            self.settings.extend(memoryview(block)[max(0, SETTINGS - offset):])

    def owner_floor(self, node, public_digest):
        # A search for physically retained, CRC-valid public records cannot
        # identify the currently selected live NVS row. Use these only to reject
        # contradictions against the authoritative USB read and byte equivalence.
        require(self.bytes == FLASH_SIZE - (PRIMARY + SLOT) and len(self.settings) == FLASH_SIZE - SETTINGS,
                "incomplete_persisted_bytes")
        require(all(k in self.settings for k in (b"mtpki/nonces0", b"mtpki/nonces1")), "missing_nonce_slot_names")
        domains, floors = set(), []
        pos = 0
        while (pos := self.settings.find(b"ZTNC\x01\0\0\0", pos)) != -1:
            record = memoryview(self.settings)[pos:pos + 64]
            pos += 4
            if len(record) != 64 or zlib.crc32(record[:60]) != struct.unpack_from("<I", record, 60)[0]:
                continue
            if struct.unpack_from("<I", record, 8)[0] != node or hashlib.sha256(record[12:44]).hexdigest() != public_digest:
                continue
            root, crc, through, generation = struct.unpack_from("<IIII", record, 44)
            require(zlib.crc32(record[12:44]) == node and root > 0 and 1 <= generation <= 16777217 and
                    through == min(0xffffffff, (generation - 1) * 256), "invalid_owner_counter_record")
            domains.add((root, crc))
            floors.append(through)
        require(len(domains) == 1 and len(floors) >= 2, "ambiguous_owner_nonce_binding")
        return domains.pop(), max(floors)


    def confirm_ordinary_contacts(self, node, owner, domain, expected):
        # Physically retained records can corroborate a complete independently
        # read authority table, but never substitute for selector resolution.
        # Require both known legacy generations and no competing local root.
        require(all(k in self.settings for k in (b"mtpki/contacts0", b"mtpki/contacts1")),
                "missing_contact_slot_names")
        generations = {}
        position = 0
        while (position := self.settings.find(b"PCS1", position)) != -1:
            row = memoryview(self.settings)[position:position + 640]
            position += 4
            if len(row) != 640 or zlib.crc32(row[:636]) != struct.unpack_from("<I", row, 636)[0]:
                continue
            version, count, generation, local = struct.unpack_from("<HHII", row, 4)
            if local != node or hashlib.sha256(row[16:48]).hexdigest() != owner:
                continue
            ident_gen, root_gen, root_crc = struct.unpack_from("<III", row, 48)
            require(version == 1 and count <= 16 and generation and ident_gen and (root_gen, root_crc) == domain,
                    "ambiguous_contact_snapshot")
            require(not any(row[60 + 36 * count:636]), "invalid_contact_snapshot_padding")
            pins = {}
            last = 0
            for idx in range(count):
                start = 60 + idx * 36
                peer = struct.unpack_from("<I", row, start)[0]
                key = hashlib.sha256(row[start + 4:start + 36]).hexdigest()
                require(2 <= peer <= 0xfffffffd and peer != node and peer > last and key not in pins.values() and key != owner,
                        "invalid_stored_peer")
                pins[peer] = key
                last = peer
            value = (ident_gen, pins)
            require(generation not in generations or generations[generation] == value, "conflicting_contact_generation")
            generations[generation] = value
        require(len(generations) >= 2 and len({v[0] for v in generations.values()}) == 1, "unconfirmed_contact_history")
        newest = max(generations)
        require(newest - 1 in generations and generations[newest][1] == expected, "unconfirmed_pin_membership")


def compare_images(before, after, original, expanded, saved, current):
    require(original["ordinary_signer_sha256"] == expanded["required_ordinary_signer"], "release_authority_pair_mismatch")
    for capture, observed in ((original, saved), (expanded, current)):
        require(capture["sha256"] == observed["full_read_sha256"] and fingerprint(observed["profile_owner_sha256"]) and
                type(observed["node"]) is int and 2 <= observed["node"] <= 0xfffffffd and
                observed["authoritative_pin_membership"] is True and observed["quiescent_acquisition"] is True and
                fingerprint(observed["physical_target_sha256"]) and reservation(observed["nonce_reserved_through"]),
                "unbound_owner_or_membership_evidence")
    require(saved["physical_target_sha256"] == current["physical_target_sha256"], "physical_target_changed")
    require((saved["node"], saved["profile_owner_sha256"]) == (current["node"], current["profile_owner_sha256"]),
            "owner_identity_changed")
    a = before.owner_floor(saved["node"], saved["profile_owner_sha256"])
    b = after.owner_floor(current["node"], current["profile_owner_sha256"])
    require(a[1] == saved["nonce_reserved_through"] and b[1] == current["nonce_reserved_through"],
            "observed_floor_conflicts_with_storage")
    require(a[0] == b[0], "owner_nonce_domain_changed")
    require(a[1] == b[1], "nonce_reservation_changed")
    left, right = pinset(saved), pinset(current)
    require(len(left) <= 16, "invalid_ordinary_pin_capacity")
    require(all(right.get(node) == key for node, key in left.items()), "saved_pin_revoked_or_rotated")
    require(left == right, "authorized_pin_membership_changed")
    require(before.digest.digest() == after.digest.digest(), "persistent_state_differs_or_replay_state_unproven")
    before.confirm_ordinary_contacts(saved["node"], saved["profile_owner_sha256"], a[0], left)
    after.confirm_ordinary_contacts(current["node"], current["profile_owner_sha256"], b[0], right)
    # Even a strict equality result is informational. A host-side comparison
    # cannot authorize restoring an owner or prove the board stayed quiescent.
    return {"status": "equivalent_persisted_state", "same_identity_restore_authorized": False,
            "archive_sha256": original["sha256"], "current_sha256": expanded["sha256"],
            "public_membership_count": len(left), "reserved_through": a[1],
            "compared_persisted_bytes": before.bytes}


def main(argv, verify):
    p = argparse.ArgumentParser(description=__doc__)
    modes = p.add_subparsers(dest="mode", required=True)
    diagnostic = modes.add_parser("compare-receipts", help="Report a refusal from existing bench observations")
    audit = modes.add_parser("pre-restore", help="Compare two verified, quiescent full-ROM streams")
    for mode in (diagnostic, audit):
        mode.add_argument("--saved-state", type=Path, required=True)
        mode.add_argument("--current-state", type=Path, required=True)
    for profile in ("ordinary", "expanded"):
        audit.add_argument("--" + profile + "-manifest", type=Path, required=True)
        for key in ("manifest-sha256", "first-rom-sha256", "second-rom-sha256"):
            audit.add_argument("--" + profile + "-" + key, required=True)
    audit.add_argument("--current-fd", type=int, required=True, help="Decrypted current image on a separate inherited descriptor")
    args = p.parse_args(argv)
    try:
        saved = json_object(args.saved_state)
        current = json_object(args.current_state)
        if args.mode == "compare-receipts":
            result = compare_receipts(saved, current)
        else:
            require(args.current_fd > 2, "separate_current_descriptor_required")
            def read(stream, state, profile):
                return verify(stream, getattr(args, profile + "_manifest"), getattr(args, profile + "_manifest_sha256"),
                              getattr(args, profile + "_first_rom_sha256"), getattr(args, profile + "_second_rom_sha256"),
                              expanded=(profile == "expanded"), observe=state.observe)
            before, after = Persisted(), Persisted()
            original = read(sys.stdin.buffer, before, "ordinary")
            with os.fdopen(os.dup(args.current_fd), "rb") as source:
                expanded = read(source, after, "expanded")
            result = compare_images(before, after, original, expanded, saved, current)
    except (OSError, ValueError, KeyError, IndexError, TypeError, AttributeError) as error:
        reason = str(error)
        # File names, input values and arbitrary receipt fields must not reach stdout.
        safe = reason if type(error) is ValueError and re.fullmatch("[a-z][a-z_]{1,79}", reason) else "incomplete_or_invalid_evidence"
        result = {"status": "refused", "same_identity_restore_authorized": False, "reason": safe}
    print(json.dumps(result))
    return 2 if result["status"] == "refused" else 3
