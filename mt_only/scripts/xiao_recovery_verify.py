#!/usr/bin/env python3
"""Check a decrypted pre-conversion 8 MiB backup against two ROM reads and a known ordinary release."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import sys

from xiao_release import BOARD, FLASH_SIZE, MAGIC, PRIMARY, SECONDARY, SLOT, require


def verify(reader, manifest_path, manifest_sha, first_sha, second_sha, *, expanded=False, observe=None):
    require(all(re.fullmatch(r"[0-9a-f]{64}", h) for h in (manifest_sha, first_sha, second_sha)),
            "Supply the three recorded SHA-256 digests")
    require(first_sha == second_sha, "The two independent device reads disagree")
    metadata = manifest_path.read_bytes()
    require(hashlib.sha256(metadata).hexdigest() == manifest_sha, "Ordinary release manifest digest differs")
    manifest = json.loads(metadata)
    version = manifest["version"]
    target = "mt-zephyr-xiao-198" if expanded else "mt-zephyr-xiao"
    require(re.fullmatch(r"\d+\.\d+\.\d+\+\d+", version) and manifest["board"] == BOARD and
            manifest.get("pio_env", "mt-zephyr-xiao") == target and
            manifest.get("private_contact_capacity", 16) == (198 if expanded else 16),
            "Unexpected signed XIAO recovery profile")
    if expanded:
        require(manifest.get("downgrade_requires_verified_recovery") is True and
                re.fullmatch(r"[0-9a-f]{64}", manifest.get("ordinary_signer_sha256", "")) and
                manifest["ordinary_signer_sha256"] != manifest["key_spki_sha256"], "Missing distinct expanded authority")
    prefix = "firmware-" + target + "-" + version
    wire = manifest["files"][prefix + "-update.bin"]
    # Older packages used a firmware- prefix for the same factory reference.
    candidates = (target + "-" + version + ".factory.bin", prefix + ".factory.bin")
    factory_names = [name for name in candidates if name in manifest["files"]]
    require(len(factory_names) == 1, "Expected one ordinary factory reference")
    factory_name = factory_names[0]
    factory = manifest["files"][factory_name]
    require(wire["purpose"] == "wireless" and 32 < wire["bytes"] <= SLOT - 8192 and
            factory["purpose"] == "fresh_install" and factory["bytes"] == FLASH_SIZE, "Unexpected release geometry")
    with (manifest_path.parent / factory_name).open("rb") as reference:
        require(hashlib.file_digest(reference, "sha256").hexdigest() == factory["sha256"],
                "Public factory reference differs from the ordinary release")
        reference.seek(0)
        bootloader = reference.read(SECONDARY)

    full = hashlib.sha256()
    offset = 0

    def consume(end, check=None):
        nonlocal offset
        while offset < end:
            block = reader.read(min(end - offset, 65536))
            require(block, "Truncated recovery stream")
            if check is not None:
                check(offset, block)
            if observe is not None:
                observe(offset, block)
            full.update(block)
            offset += len(block)

    def original_loader(at, data):
        require(data == bootloader[at:at + len(data)], "Backup bootloader differs from the ordinary release")

    def erased(at, data):
        require(data == b"\xff" * len(data), f"Unexpected programmed bytes in the recovery image near {at:#x}")

    consume(SECONDARY, original_loader)
    consume(PRIMARY - 16)
    consume(PRIMARY, erased)  # No pending secondary trial.
    application = hashlib.sha256()
    consume(PRIMARY + wire["bytes"], lambda at, data: application.update(data))
    require(application.hexdigest() == wire["sha256"], "Backup primary differs from the enrolled ordinary release")
    end = PRIMARY + SLOT
    consume(end - 24)
    consume(end - 23, lambda at, data: require(data == b"\x01", "Ordinary application was not confirmed"))
    consume(end - 16)
    consume(end, lambda at, data: require(data == MAGIC[at - (end - 16):at - (end - 16) + len(data)],
                                         "Unexpected primary slot trailer"))
    consume(0x480000)
    consume(0x488000, None if expanded else erased)  # The ordinary backup must predate conversion.
    consume(0x7f0000)
    settings_present = False

    def settings(at, data):
        nonlocal settings_present
        settings_present |= data != b"\xff" * len(data)

    consume(FLASH_SIZE, settings)
    require(not reader.read(1) and settings_present, "Expected an enrolled, complete 8 MiB ordinary backup")
    require(full.hexdigest() == first_sha, "Decrypted recovery bytes differ from the independent ROM reads")
    profile = "expanded" if expanded else "ordinary"
    result = {"verified_bytes": offset, "sha256": first_sha, profile + "_version": version,
              profile + "_manifest_sha256": manifest_sha, profile + "_signer_sha256": manifest["key_spki_sha256"]}
    if expanded:
        result["required_ordinary_signer"] = manifest["ordinary_signer_sha256"]
    return result


if __name__ == "__main__":
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if sys.argv[1:2] in (["pre-restore"], ["compare-receipts"]):
        from xiao_restore_comparison import main
        sys.exit(main(sys.argv[1:], verify))
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ordinary-manifest", type=Path, required=True)
    p.add_argument("--manifest-sha256", required=True)
    p.add_argument("--first-rom-sha256", required=True)
    p.add_argument("--second-rom-sha256", required=True)
    args = p.parse_args()
    try:
        print(json.dumps(verify(sys.stdin.buffer, args.ordinary_manifest, args.manifest_sha256,
                                args.first_rom_sha256, args.second_rom_sha256)))
    except (OSError, ValueError, KeyError, TypeError) as error:
        p.exit(1, f"{error}\n")
