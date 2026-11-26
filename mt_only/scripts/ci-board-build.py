#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build one beta hardware profile from this checkout and record linked memory usage."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from zipfile import ZipFile

APP = Path(__file__).resolve().parents[1]
EPOCH = "1795713761"
MCUBOOT_COMMIT = "aa32eaaad41cc345e6f6e6633368a4766fe23c25"
T096_FLAGS = {
    "t096-680": ["--initial-release", "--normal-tx", "--stock-pki-trial",
                 "--lto-runtime", "--t096-fresh-domain"],
    "t096-728": ["--t096-full-companion"],
}
V4_BOARDS = {"v4.2-r2": "heltec_wifi_lora32_v4", "v4.3-r2": "heltec_wifi_lora32_v43",
             "v4.3-r8": "heltec_wifi_lora32_v4_r8"}
ROUTER = "t096-router-680"
PROFILES = [*T096_FLAGS, ROUTER, "t114-v2", "rak4631-mini", *V4_BOARDS, "xiao-ordinary"]
KITS = {
    "rak": ("rak4631-rak19003-mini-firmware", "0a5aafba4d1a1bbf71c981e57f8b2ec1c6f48eeb2b4f0a86118894659d006950"),
    "v4": ("heltec-v4-build-sources", "e08a9e73243e9a8e9a15a92638027d04d50c1d26a9e0fa4868dac4c279545efd"),
}


def verify_dependencies(deps, mcuboot):
    def revision(directory):
        return subprocess.check_output(["git", "-C", directory, "rev-parse", "HEAD"], text=True).strip()

    for project in json.loads((APP / "dependency-lock.json").read_text()):
        root = deps / project["path"]
        if revision(root) != project["commit"]:
            raise ValueError(f"Dependency checkout differs from lock: {project['path']}")
        for prefix, entries in ((root, project["overlay"]), (root / "zephyr/blobs", project.get("blobs", []))):
            for entry in entries:
                if hashlib.sha256((prefix / entry["path"]).read_bytes()).hexdigest() != entry["sha256"]:
                    raise ValueError(f"Dependency digest differs from lock: {project['path']}/{entry['path']}")
    if mcuboot and revision(mcuboot) != MCUBOOT_COMMIT:
        raise ValueError("MCUboot checkout differs from the beta pin")


def board_inputs(family, directory):
    """Take only board descriptions and configuration from the audited beta kits."""
    stem, digest = KITS[family]
    name = f"zephtastic-0.1-beta-{stem}.zip"
    archive = directory / name
    subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--retry", "3",
                    "--output", archive, f"https://github.com/Nesseth-Inc/Zephtastic/releases/download/0.1-beta/{name}"],
                   check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise ValueError(f"Board input kit digest differs from 0.1-beta: {name}")
    prefixes = ("inputs/",) if family == "rak" else ("board-root/boards/",)
    extras = {"build/rx-only.conf", "build/native-usb.overlay"} if family == "v4" else set()
    with ZipFile(archive) as kit:
        for member in kit.infolist():
            path = Path(member.filename)
            if member.is_dir() or not (member.filename.startswith(prefixes) or member.filename in extras):
                continue
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"Invalid board input path: {path}")
            target = directory / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(kit.read(member))


def usage(used, capacity):
    if not 0 < used <= capacity:
        raise ValueError(f"Invalid memory usage: {used} / {capacity} bytes")
    return {"used_bytes": used, "capacity_bytes": capacity, "free_bytes": capacity - used}


def linker_usage(log, region):
    pattern = rf"^\s*{region}:\s+(\d+) (B|KB|MB)\s+(\d+) (B|KB|MB)\s+"
    rows = re.findall(pattern, log, re.MULTILINE)
    if not rows:
        raise ValueError(f"Linker did not report region {region}")
    used, used_unit, capacity, unit = rows[-1]
    scale = {"B": 1, "KB": 1024, "MB": 1024**2}
    return usage(int(used) * scale[used_unit], int(capacity) * scale[unit])


def flash_capacity(zephyr, simple_boot):
    dts = re.sub(r"/\*.*?\*/", "", (zephyr / "zephyr.dts").read_text(), flags=re.S)
    if simple_boot:
        # The V4 image starts at zero and uses no OTA slot. Its first live user
        # storage region is the safe flash boundary, as in its release recipe.
        if "CONFIG_ESP_SIMPLE_BOOT=y" not in (zephyr / ".config").read_text().splitlines():
            raise ValueError("V4 flash geometry requires the simple-boot build")
        label = "storage_partition"
    else:
        chosen = re.search(r"zephyr,code-partition\s*=\s*&(\w+);", dts)
        if chosen is None:
            raise ValueError("Missing chosen code partition")
        label = chosen[1]
    node = re.search(rf"\b{label}:\s*partition@[\da-f]+\s*\{{([^{{}}]+)\}}", dts)
    cells = re.search(r"\breg\s*=\s*<\s*(\w+)\s+(\w+)\s*>", node[1]) if node else None
    if cells is None:
        raise ValueError(f"Missing flash geometry for {label}")
    return int(cells[1 if simple_boot else 2], 0)


def build(args):
    out, deps, sdk = args.build.resolve(), args.deps.resolve(), args.sdk.resolve()
    profile = args.profile
    if out.exists():
        raise ValueError("Use a new build directory")
    if (sdk / "sdk_version").read_text().strip() != "1.0.1":
        raise ValueError("Zephyr SDK 1.0.1 is required")
    mcuboot = args.mcuboot.resolve() if args.mcuboot and profile == "xiao-ordinary" else None
    if profile == "xiao-ordinary" and not mcuboot:
        raise ValueError("The ordinary XIAO build requires --mcuboot")
    verify_dependencies(deps, mcuboot)
    py = sys.executable
    z, mods = deps / "zephyr", deps / "modules"
    env = dict(os.environ, ZEPHYR_BASE=str(z), ZEPHYR_SDK_INSTALL_DIR=str(sdk),
               ZEPHYR_TOOLCHAIN_VARIANT="zephyr", SOURCE_DATE_EPOCH=EPOCH,
               PYTHONDONTWRITEBYTECODE="1", PATH=str(Path(py).parent) + os.pathsep + os.environ["PATH"])
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zt-board-", dir=out.parent) as temp:
        kit = Path(temp)
        if profile in T096_FLAGS:
            commands = [[py, APP / "scripts/build-local.py", "t096", "--zephyr", z, "--modules", mods,
                         "--nordic-modules", mods, "--sdk", sdk, "--source-date-epoch", EPOCH,
                         "--build", out, "--jobs", str(args.jobs), *T096_FLAGS[profile]]]
        elif profile == "xiao-ordinary":
            commands = [[py, APP / "scripts/xiao_release.py", "build", "--zephyr", z, "--modules", mods,
                         "--mcuboot", mcuboot, "--sdk", sdk, "--source-date-epoch", EPOCH,
                         "--build", out, "--public-key", APP / "boards/keys/xiao-ordinary-release-v1.pem"]]
        else:
            modules = ["crypto/mbedtls", "crypto/tf-psa-crypto", "lib/loramac-node"]
            if profile in V4_BOARDS:
                board_inputs("v4", kit)
                board = V4_BOARDS[profile] + "/esp32s3/procpu"
                modules += ["hal/espressif", "hal/xtensa"]
                options = [f"-DBOARD_ROOT={kit}/board-root", f"-DEXTRA_CONF_FILE={kit}/build/rx-only.conf",
                           f"-DDTC_OVERLAY_FILE={kit}/build/native-usb.overlay"]
            else:
                modules += ["hal/cmsis", "hal/cmsis_6", "hal/nordic"]
                if profile == ROUTER:
                    board = "heltec_t096/nrf52840"
                    options = [f"-DCONF_FILE={APP}/boards/t096-router.conf",
                               f"-DEXTRA_CONF_FILE={APP}/boards/t096-fresh-domain.conf",
                               f"-DDTC_OVERLAY_FILE={APP}/boards/t096.overlay",
                               f"-DEXTRA_DTC_OVERLAY_FILE={APP}/boards/t096-fresh-domain.overlay;{APP}/boards/t096-router.overlay"]
                elif profile == "rak4631-mini":
                    board_inputs("rak", kit)
                    board = "rak4631/nrf52840"
                    options = [f"-DEXTRA_CONF_FILE={kit}/inputs/mini.conf",
                               f"-DDTC_OVERLAY_FILE={kit}/inputs/mini-no-rtc.overlay"]
                else:
                    board = "heltec_t114_v2/nrf52840/uf2"
                    options = ["-DCONFIG_MT_ALLOW_TX=y", "-DCONFIG_MT_NORMAL_TX_DEFAULT=n",
                               f"-DDTC_OVERLAY_FILE={APP}/boards/t114-v2.overlay"]
            commands = [["cmake", "-GNinja", "-S", APP, "-B", out, "-DBOARD=" + board,
                         "-DPython3_EXECUTABLE=" + py, "-DZEPHYR_MODULES=" + ";".join(str(mods / m) for m in modules),
                         *options], ["cmake", "--build", out, "--parallel", str(args.jobs)]]
        log_path = out.parent / (out.name + ".log")
        with log_path.open("w") as log:
            for cmd in commands:
                print("Building", profile, flush=True)
                try:
                    subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
                except subprocess.CalledProcessError:
                    log.flush()
                    print("\n".join(log_path.read_text(errors="replace").splitlines()[-80:]), file=sys.stderr)
                    raise
    app = out / "app" if profile == "xiao-ordinary" else out
    zephyr = app / "zephyr"
    log = (app / "build.log" if (app / "build.log").is_file() else log_path).read_text()
    esp = profile in V4_BOARDS or profile == "xiao-ordinary"
    if esp:
        flash = usage((zephyr / "zephyr.bin").stat().st_size, flash_capacity(zephyr, profile in V4_BOARDS))
        ram = linker_usage(log, "dram0_0_seg")
        if profile in V4_BOARDS and "CONFIG_MT_ALLOW_TX=y" in (zephyr / ".config").read_text().splitlines():
            raise ValueError("V4 beta must be receive-only")
    else:
        flash, ram = linker_usage(log, "FLASH"), linker_usage(log, "RAM")
        if not (zephyr / "zephyr.uf2").is_file():
            raise ValueError("Nordic build is missing its UF2")
    if profile == ROUTER:
        enabled = set((zephyr / ".config").read_text().splitlines())
        required = ["MT_T096_ROUTER_ONLY", "MT_T096_OWNER_TX", "MT_ALLOW_TX", "MT_NORMAL_ENDPOINT",
                    "MT_NORMAL_TX_DEFAULT", "MT_PKI_STOCK_TRIAL", "MT_PKI_TARGET_RUNTIME",
                    "MT_PKI_FRESH_DOMAIN_ONLY", "MT_RF_ADMIN_GET", "USB_DEVICE_STACK_NEXT",
                    "CDC_ACM_SERIAL_INITIALIZE_AT_BOOT", "ENTROPY_NRF5_BIAS_CORRECTION"]
        omitted = ["BT", "MT_BLE_NATIVE_STORE", "MT_DISPLAY", "MT_GNSS_T096", "GNSS", "MT_ENVIRONMENT_SENSORS"]
        if flash["capacity_bytes"] != 680 * 1024:
            raise ValueError("Router must retain the 680 KiB application partition")
        for symbol in required + omitted:
            if (f"CONFIG_{symbol}=y" in enabled) != (symbol in required):
                raise ValueError(f"Incorrect router configuration for {symbol}")
    report = {"profile": profile, "commit": subprocess.check_output(
        ["git", "-C", APP, "rev-parse", "HEAD"], text=True).strip(),
        "flash_measure": ("simple-boot bytes / bytes before storage" if profile in V4_BOARDS else
                          "unsigned application bytes / code partition" if esp else "linker FLASH"),
        "ram_measure": "linker internal DRAM" if esp else "linker static RAM",
        "flash": flash, "ram": ram}
    (out / "memory.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if summary := os.getenv("GITHUB_STEP_SUMMARY"):
        with open(summary, "a") as stream:
            print(f"| Profile | Flash free / total (bytes) | Static RAM free / total (bytes) |\n"
                  f"| --- | ---: | ---: |\n| {profile} | {flash['free_bytes']:,} / {flash['capacity_bytes']:,} "
                  f"| {ram['free_bytes']:,} / {ram['capacity_bytes']:,} |", file=stream)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=PROFILES)
    for option in ("deps", "sdk", "build"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--mcuboot", type=Path)
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=4)
    build(parser.parse_args())
