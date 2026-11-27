#!/usr/bin/env python3
"""Compare ZT telemetry schedules, RF admission and wire payloads with Meshtastic 2.8.2."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
ROOT = APP.parent
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

REVISION = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
PINS = {
    "mesh/Default.cpp": "a0a46f1b5a6173fc0365f4cb50353fb52ae6748ee2e036ddec4a7900918f9443",
    "mesh/Default.h": "0a468de454cb7da41536b90715cde20d506b2430d3577310a32347b67fed0116",
    "airtime.cpp": "95c8402594cbc1534b06d4be178cdc5757cba752d7e606da3a3bb9ed7e6078e0",
    "airtime.h": "c9a3388e8cedca600438e23d32d1cbbfd4a0a24bda621c5bff12b6e576d04fa2",
    "modules/Telemetry/BaseTelemetryModule.h": "39388ac3e39b457e4045fe6572b07413b2657b1ce3b911d905bfb4e340b72c52",
    "modules/Telemetry/DeviceTelemetry.cpp": "481f3a5be01cf60a8eeb420ec67d710f7cb8daf6086b1ce8b96920ebcc7fea67",
    "modules/Telemetry/EnvironmentTelemetry.cpp": "e3600610c6ca85437286c85f608d1a570cefd5e58f0696d9e2dd613d0a0d3b1f",
}


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def admission_clause(source):
    # Capture the role, channel and duty-cycle conjuncts of the actual mesh branch.
    start = source.index("airTime->isTxAllowedChannelUtil(")
    return source[start:source.index(") {", start)]


def prepare(stock, out):
    units = {}
    for name, expected in PINS.items():
        data = (stock / "src" / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"{name} differs from stock {REVISION}")
        units[name] = data.decode()
    for name in ("Default.cpp", "Default.h"):
        (out / name).write_text(units["mesh/" + name])
    device = units["modules/Telemetry/DeviceTelemetry.cpp"]
    env = units["modules/Telemetry/EnvironmentTelemetry.cpp"]
    base = units["modules/Telemetry/BaseTelemetryModule.h"]
    role = base.index("bool isSensorOrRouterRole() const")
    (out / "role.inc").write_text(base[role:base.index("\n    }", role) + 6])
    (out / "channel_limits.inc").write_text("\n".join(
        line for line in units["airtime.h"].splitlines()
        if re.match(r"\s*uint8_t (?:polite|max)_channel_util_percent =", line)))
    (out / "admission.inc").write_text(
        "bool device() const { const bool isImpoliteRole = isSensorOrRouterRole(); return " +
        admission_clause(method(device, "int32_t DeviceTelemetryModule::runOnce()")) +
        "; }\nbool environment() const { return " +
        admission_clause(method(env, "int32_t EnvironmentTelemetryModule::runOnce()")) + "; }\n")
    (out / "reports.inc").write_text("\n".join((
        next(line for line in device.splitlines() if line.startswith("#define MAGIC_USB_BATTERY_LEVEL ")),
        method(units["airtime.cpp"], "bool AirTime::isTxAllowedChannelUtil("),
        method(device, "meshtastic_Telemetry DeviceTelemetryModule::getDeviceTelemetry()"),
        method(env, "bool EnvironmentTelemetryModule::getEnvironmentTelemetry("),
    )))


def run_profile(out, abi):
    proto = ROOT / "zephcore/bench/upstream-differential/source"
    vendor = ROOT / "zephcore/adapters/bench/vendor"
    mono = ROOT / "zephcore/lib/monocypher"
    common = ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *abi]
    default_obj = out / "stock-default.o"
    subprocess.run([*host.cxx(), "-std=c++17", *common, "-DUSERPREFS_EVENT_MODE=0",
                    "-I", out, "-I", APP / "tests/module_intervals/stubs", "-c", HERE / "default.cpp",
                    "-o", default_obj], check=True)
    flags = [*common, "-DCONFIG_MT_TAK_TRACKER=1", "-I", out, "-I", proto / "nanopb",
             "-I", APP / "src", "-I", APP / "tests", "-I", mono, "-I", vendor,
             *host.crypto_compile_flags()]
    objects = [default_obj]
    for src in (mono / "monocypher.c", mono / "monocypher-ed25519.c",
                *(proto / "nanopb" / name for name in ("pb_common.c", "pb_encode.c", "pb_decode.c"))):
        obj = out / (src.stem + ".o")
        subprocess.run([*host.cc(), "-std=c11", *flags, "-c", src, "-o", obj], check=True)
        objects.append(obj)
    exe = out / "telemetry"
    subprocess.run([*host.cxx(), "-std=c++17", *flags, HERE / "cases.cpp", APP / "src/Protobuf.cpp",
                    vendor / "MtCodec.cpp", vendor / "MtSignature.cpp", APP / "src/pki/PkiIdentity.cpp",
                    APP / "src/pki/PkiSigner.cpp", *objects, *host.crypto_link_flags(), "-o", exe], check=True)
    output = subprocess.check_output([exe], text=True, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
    return json.loads(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    profiles = {}
    with tempfile.TemporaryDirectory(prefix="zt-stock-telemetry-") as tmp:
        out = Path(tmp)
        (out / "meshtastic").symlink_to(ROOT / "zephcore/bench/upstream-differential/source",
                                       target_is_directory=True)
        prepare(args.stock, out)
        for name, flags in (("default", []), ("short-enums", ["-fshort-enums"])):
            profiles[name] = run_profile(out, flags)
    count = sum(sum(rows.values()) for rows in profiles.values())
    receipt = {"stock_revision": REVISION, "comparisons": {"telemetry": count},
               "profiles": profiles, "hardware": False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt))


if __name__ == "__main__":
    main()
