#!/usr/bin/env python3
"""Build/run actual MQTT-enabled Service and SettingsCommit native fixtures."""
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

APP = ROOT / "mt_only"
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"
VENDOR = ROOT / "zephcore/adapters/bench/vendor"
MONO = ROOT / "zephcore/lib/monocypher"


def run(command):
    print("$", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True)


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-gateway-service-") as tmp:
        build = Path(tmp)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                  "-DPB_ENABLE_MALLOC", "-I" + str(build), "-I" + str(NANOPB), "-I" + str(APP / "src"),
                  "-I" + str(APP / "tests"), "-I" + str(VENDOR), "-I" + str(MONO),
                  *host.crypto_compile_flags()]
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        c_sources = (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c",
                     MONO / "monocypher.c", MONO / "monocypher-ed25519.c")
        profiles = (("gateway", ["-DCONFIG_MT_MQTT_GATEWAY=1"]),
                    ("proxy-only", []))
        enums = (("default-enums", []), ("short-enums", ["-fshort-enums"]))
        for profile, profile_flags in profiles:
            for label, enum_flags in enums:
                case = profile + "-" + label
                objects = []
                for source in c_sources:
                    obj = build / (case + "-" + source.stem + ".o")
                    run([*cc, "-std=c11", *common, *profile_flags, *enum_flags, "-Wall", "-Wextra", "-c", source, "-o", obj])
                    objects.append(obj)
                exe = build / ("mqtt-gateway-service-" + case)
                run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *profile_flags, *enum_flags, *common,
                     APP / "tests/mqtt_gateway_service_tests.cpp", APP / "src/Protobuf.cpp",
                     VENDOR / "MtCodec.cpp", VENDOR / "MtSignature.cpp", *objects,
                     *host.crypto_link_flags(), "-o", exe])
                result = subprocess.run([str(exe)], check=False, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, env=env)
                print(case + ":", flush=True)
                print(result.stdout, end="")
                if result.returncode != 0:
                    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
