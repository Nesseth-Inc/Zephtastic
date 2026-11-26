#!/usr/bin/env python3
"""Build or package the signed, full-featured XIAO ESP32-S3 companion release."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

APP = Path(__file__).resolve().parents[1]
BOARD = "xiao_esp32s3/esp32s3/procpu"
FLASH_SIZE = 0x800000
SECONDARY = 0x10000
PRIMARY = 0x1d0000
SLOT = 0x1c0000
MAGIC = bytes.fromhex("77c295f360d2ef7f3552500f2cb67980")
GEOMETRY = {
    "boot_partition": (0, 0x10000), "sys_partition": (0x390000, 0x10000),
    "slot0_partition": (PRIMARY, SLOT), "slot1_partition": (SECONDARY, SLOT),
    "scratch_partition": (0x3e0000, 0x10000), "storage_partition": (0x3b0000, 0x30000),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def public_der(path):
    lines = path.read_bytes().strip().splitlines()
    require(len(lines) >= 3 and lines[0] == b"-----BEGIN PUBLIC KEY-----" and
            lines[-1] == b"-----END PUBLIC KEY-----", "Supply the P-256 public key in SPKI PEM format")
    der = base64.b64decode(b"".join(lines[1:-1]), validate=True)
    prefix = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d03010703420004")
    require(len(der) == 91 and der.startswith(prefix), "Expected an uncompressed P-256 public key")
    return der


EXPANDED = "MT_XIAO_EXPANDED_CONTACTS"


def profile_authority(public_key, expanded, ordinary_public_key):
    if not expanded:
        require(ordinary_public_key is None, "An ordinary signer comparison requires the 198-key profile")
        return None
    require(ordinary_public_key is not None, "Supply the established ordinary release public key for comparison")
    ordinary_der = public_der(ordinary_public_key)
    require(ordinary_der != public_der(public_key), "The 198-key profile requires its own release authority")
    return sha(ordinary_der)


def check_config(build, enabled, disabled):
    options = set((build / "zephyr/.config").read_text().splitlines())
    for option in enabled:
        require("CONFIG_" + option + "=y" in options, f"{build.name} requires {option}")
    for option in disabled:
        require("CONFIG_" + option + "=y" not in options, f"{build.name} has incompatible {option}")


def check_layout(build, include_settings=False, expanded=False):
    dts = re.sub(r"/\*.*?\*/", "", (build / "zephyr/zephyr.dts").read_text(), flags=re.S)
    regions = {**GEOMETRY, "flash0": (0, FLASH_SIZE)}
    if include_settings:
        regions.update(mt_storage=(0x7f0000, 0x10000), mt_reset_floor=(0x7ef000, 0x1000),
                       mt_sat_partition=(0x400000, 0x80000))
        require(re.search(r"zephyr,settings-partition\s*=\s*&mt_storage;", dts), "Unexpected settings owner")
    if expanded:
        regions["mt_pki_contacts_partition"] = (0x480000, 0x8000)
    else:
        require("mt_pki_contacts_partition:" not in dts, "Unexpected expanded contact partition")
    require(re.search(r"zephyr,code-partition\s*=\s*&slot0_partition;", dts), "Unexpected application slot")
    for label, expected in regions.items():
        node = re.search(r"\b" + label + r":\s+[a-z-]+@[0-9a-f]+\s*\{([^{}]+)", dts)
        reg = re.search(r"\breg\s*=\s*<\s*(0x[\da-f]+|\d+)\s+(0x[\da-f]+|\d+)\s*>", node[1]) if node else None
        require(reg and tuple(int(value, 0) for value in reg.groups()) == expected, f"Unexpected {label} in {build}")
        if label == "flash0":
            require(re.search(r"write-block-size\s*=\s*<\s*0x4\s*>", node[1]) and
                    re.search(r"erase-block-size\s*=\s*<\s*0x1000\s*>", node[1]),
                    "Flash write or erase geometry changed")
    options = set((build / "zephyr/.config").read_text().splitlines())
    require(f'CONFIG_BOARD_TARGET="{BOARD}"' in options, "Unexpected board target")
    if not include_settings:
        require("CONFIG_MCUBOOT_LOGICAL_SECTOR_SIZE=0x10000" in options and
                "CONFIG_MCUBOOT_VERIFY_LOGICAL_SECTORS=y" in options,
                "XIAO scratch swaps require 64 KiB logical erase blocks")
        require({"CONFIG_BOOT_MAX_IMG_SECTORS=448", "CONFIG_MCUBOOT_BOOT_MAX_ALIGN=4"} <= options,
                "Configured swap sectors or trailer alignment changed")


def check_build(root, public_key, require_ble=True, expanded=False, *, newly_built=False):
    app, boot = root / "app", root / "boot"
    features = ["BOOTLOADER_MCUBOOT", "MT_SIGNED_WIFI_OTA", "IMG_ERASE_PROGRESSIVELY", "MT_STORE_FORWARD_PSRAM",
                "MT_PKI_TARGET_RUNTIME", "MT_BROWSER_MESSAGING", "MT_SATELLITE_HISTORY", "MT_PAGED_SATELLITES"]
    if require_ble:
        features.append("MT_SIGNED_BLE_OTA")
    if expanded:
        features.extend([EXPANDED, "MT_XIAO_LARGE_NODEDB", "MT_SATELLITE_INITIALIZE_ERASED"])
    check_config(app, features, ["ESP_SIMPLE_BOOT", "MCUMGR_TRANSPORT_UDP"] + ([] if expanded else [EXPANDED]))
    if newly_built:
        # Packaging still accepts earlier verified build directories.
        opt = ["MT_STOCK_IDENTITY_IMPORT"]
        check_config(app, [] if expanded else opt, opt if expanded else [])
    check_config(boot, ["BOOT_VALIDATE_SLOT0", "BOOT_SWAP_USING_SCRATCH", "BOOT_SIGNATURE_TYPE_ECDSA_P256",
                        "MCUBOOT_DOWNGRADE_PREVENTION", "BOOT_VERSION_CMP_USE_BUILD_NUMBER"],
                 ["BOOT_SIGNATURE_TYPE_NONE", "BOOT_UPGRADE_ONLY", "MCUBOOT_SERIAL"])
    check_layout(boot)
    check_layout(app, include_settings=True, expanded=expanded)
    raw = (app / "zephyr/zephyr.bin").read_bytes()
    loader = (boot / "zephyr/zephyr.bin").read_bytes()
    der = public_der(public_key)
    require(der in loader and der[-65:] in raw and bytes.fromhex(sha(der)) in raw, "Signing authority differs from linked firmware")
    require(len(raw) < SLOT - 8192 and raw[:32] == bytes(32) and raw[32:36] == struct.pack("<I", 0xace637d3),
            "Unexpected ordinary firmware image")
    require(0 < len(loader) <= SECONDARY and loader[0] == 0xe9, "Unexpected XIAO bootloader")
    return raw, loader


def build_images(root, zephyr, modules, mcuboot, sdk, public_key, epoch, require_ble=True, *,
                 expanded=False, ordinary_public_key=None):
    ordinary_hash = profile_authority(public_key, expanded, ordinary_public_key)
    public_der(public_key)
    app, boot = root / "app", root / "boot"
    env = dict(os.environ, ZEPHYR_BASE=str(zephyr.resolve()), ZEPHYR_SDK_INSTALL_DIR=str(sdk.resolve()),
               ZEPHYR_TOOLCHAIN_VARIANT="zephyr", SOURCE_DATE_EPOCH=str(epoch), PYTHONDONTWRITEBYTECODE="1")
    def run(argv):
        subprocess.run([str(value) for value in argv], env=env, check=True)
    run([sys.executable, APP / "scripts/build-local.py", "xiao", "--xiao-full-companion", "--paged-satellites",
         *(["--xiao-expanded-contacts"] if expanded else []),
         "--xiao-signed-ota", mcuboot.resolve(), "--ota-public-key", public_key.resolve(), "--zephyr", zephyr.resolve(),
         "--modules", modules.resolve(), "--sdk", sdk.resolve(), "--build", app, "--jobs", "4", "--source-date-epoch", epoch])
    names = ("crypto/mbedtls", "crypto/tf-psa-crypto", "lib/loramac-node", "hal/espressif", "hal/xtensa")
    paths = [(modules / name).resolve() for name in names] + [mcuboot.resolve(), APP / "boot/xiao-swap"]
    run(["cmake", "-G", "Ninja", "-S", mcuboot / "boot/zephyr", "-B", boot, "-DBOARD=" + BOARD,
         "-DPython3_EXECUTABLE=" + sys.executable, "-DZEPHYR_MODULES=" + ";".join(map(str, paths)),
         "-DEXTRA_CONF_FILE=" + str(APP / "boards/xiao-signed-boot.conf"),
         "-DDTC_OVERLAY_FILE=" + str(APP / "boards/xiao-signed-ota.overlay"),
         '-DCONFIG_BOOT_SIGNATURE_KEY_FILE="' + str(public_key.resolve()) + '"'])
    run(["cmake", "--build", boot, "--parallel", "4"])
    binaries = check_build(root, public_key, require_ble, expanded, newly_built=True)
    if expanded:
        (root / "ordinary-signer.sha256").write_text(ordinary_hash + "\n")
    return binaries


def make_package(root, public_key, signed_file, dest, *, expanded=False, ordinary_public_key=None):
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    ordinary_hash = profile_authority(public_key, expanded, ordinary_public_key)
    if expanded:
        require((root / "ordinary-signer.sha256").read_text().strip() == ordinary_hash,
                "Ordinary release authority differs from the one checked at build time")
    raw, loader = check_build(root, public_key, expanded=expanded)
    data = signed_file.read_bytes()
    require(32 < len(data) <= SLOT - 8192, "Signed image exceeds the secondary slot")
    magic, addr, header, protected, body, flags = struct.unpack_from("<IIHHII", data)
    require((magic, addr, header, protected, flags) == (0x96f3b83d, 0, 32, 0, 0) and
            body == len(raw) - 32 and data[32:len(raw)] == raw[32:], "Signed payload differs from this build")
    off = len(raw)
    require(off + 4 < len(data), "Missing MCUboot TLVs")
    tag, total = struct.unpack_from("<HH", data, off)
    require(tag == 0x6907 and off + total == len(data), "Supply an unpadded signed update image")
    tlvs = {}
    off += 4
    while off < len(data):
        require(len(data) - off >= 4, "Incomplete TLV header")
        tag, count = struct.unpack_from("<HH", data, off)
        off += 4
        require(tag not in tlvs and count <= len(data) - off, "Invalid MCUboot TLV")
        tlvs[tag] = data[off:off + count]
        off += count
    der = public_der(public_key)
    key_id = hashlib.sha256(der).digest()
    require(der in loader and der[-65:] in raw and key_id in raw, "Public key changed after build validation")
    require(set(tlvs) == {1, 0x10, 0x22} and tlvs[1] == key_id and len(tlvs[0x10]) == 32,
            "Signed metadata must match the device's ECDSA key-hash format")
    signature = tlvs[0x22]
    require(8 <= len(signature) <= 72 and utils.encode_dss_signature(*utils.decode_dss_signature(signature)) == signature,
            "Expected a canonical DER P-256 signature")
    body_hash = hashlib.sha256(data[:len(raw)]).digest()
    require(body_hash == tlvs[0x10], "Payload digest differs from signed metadata")
    try:
        serialization.load_der_public_key(der).verify(signature, body_hash, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    except InvalidSignature as error:
        raise ValueError("MCUboot signature verification failed") from error
    version = "{}.{}.{}+{}".format(*struct.unpack_from("<BBHI", data, 20))
    require(len(version) < 18, "Version exceeds ordinary PhoneAPI metadata")
    pending = bytearray(data.ljust(SLOT, b"\xff"))
    pending[-16:] = MAGIC
    confirmed = pending.copy()
    confirmed[-24] = 1
    factory = bytearray(b"\xff") * FLASH_SIZE
    factory[:len(loader)] = loader
    factory[PRIMARY:PRIMARY + SLOT] = confirmed
    # Both mobile clients match the connected companion's exact MyNodeInfo.pio_env.
    pio_env = "mt-zephyr-xiao-198" if expanded else "mt-zephyr-xiao"
    target = pio_env + "-" + version
    artifacts = {
        "firmware-" + target + "-update.bin": ("wireless", data),
        "firmware-" + target + ".bin": ("wireless_apple", data),
        # Both mobile pickers exclude mt-*. Apple's local parser would accept
        # firmware-<target>-<version>.factory.bin as an ordinary update.
        target + "-usb-update.bin": ("stock_usb_secondary", pending),
        target + ".factory.bin": ("fresh_install", factory),
    }
    require(not dest.exists(), "Choose a new release output directory")
    dest.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".xiao-release-", dir=dest.parent))
    try:
        manifest = {"board": BOARD, "version": version, "key_spki_sha256": key_id.hex(),
                    "raw_application_sha256": sha(raw), "bootloader_sha256": sha(loader), "files": {}}
        manifest.update(pio_env=pio_env, private_contact_capacity=198 if expanded else 16)
        if expanded:
            manifest["ordinary_signer_sha256"] = ordinary_hash
            manifest["downgrade_requires_verified_recovery"] = True
        for filename, (purpose, content) in artifacts.items():
            (work / filename).write_bytes(content)
            manifest["files"][filename] = {"purpose": purpose, "bytes": len(content), "sha256": sha(content)}
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        require(not dest.exists(), "Release directory appeared during packaging")
        work.rename(dest)
    finally:
        if work.exists():
            shutil.rmtree(work)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="operation", required=True)
    build = actions.add_parser("build", help="Compile the ordinary application and release bootloader")
    pack = actions.add_parser("pack", help="Verify a signed application and emit the release artifacts")
    for cmd in (build, pack):
        cmd.add_argument("--build", type=Path, required=True)
        cmd.add_argument("--public-key", type=Path, required=True)
        cmd.add_argument("--xiao-expanded-contacts", action="store_true", help="Select the separately signed 198-key profile")
        cmd.add_argument("--ordinary-public-key", type=Path, help="Established 16-key release public key for comparison")
    for field in ("zephyr", "modules", "mcuboot", "sdk"):
        build.add_argument("--" + field, type=Path, required=True)
    build.add_argument("--source-date-epoch", type=int, required=True)
    pack.add_argument("--signed", type=Path, required=True)
    pack.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.operation == "build":
            build_images(args.build.resolve(), args.zephyr, args.modules, args.mcuboot, args.sdk,
                         args.public_key, args.source_date_epoch, expanded=args.xiao_expanded_contacts,
                         ordinary_public_key=args.ordinary_public_key)
        else:
            result = make_package(args.build.resolve(), args.public_key, args.signed, args.out.resolve(),
                                  expanded=args.xiao_expanded_contacts, ordinary_public_key=args.ordinary_public_key)
            print(json.dumps(result, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
