#!/usr/bin/env python3
"""Build/run Companion proxy integration without the WiFi-only gateway macro."""
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
    subprocess.run(list(map(str, command)), check=True)


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-client-proxy-service-") as temporary:
        build = Path(temporary)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                  "-DPB_ENABLE_MALLOC", "-I" + str(build), "-I" + str(NANOPB),
                  "-I" + str(APP / "src"), "-I" + str(APP / "tests"),
                  "-I" + str(VENDOR), "-I" + str(MONO), *host.crypto_compile_flags()]
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        c_sources = (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c",
                     MONO / "monocypher.c", MONO / "monocypher-ed25519.c")
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            objects = []
            for source in c_sources:
                obj = build / (label + "-" + source.stem + ".o")
                run([*cc, "-std=c11", *common, *enum_flags, "-Wall", "-Wextra",
                     "-c", source, "-o", obj])
                objects.append(obj)
            executable = build / ("mqtt-client-proxy-service-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags,
                 *common, APP / "tests/mqtt_client_proxy_service_tests.cpp",
                 APP / "src/Protobuf.cpp", VENDOR / "MtCodec.cpp",
                 VENDOR / "MtSignature.cpp", *objects, *host.crypto_link_flags(),
                 "-o", executable])
            subprocess.run([str(executable)], check=True, env=env)
    print("Companion MQTT client-proxy integration tests PASS without CONFIG_MT_MQTT_GATEWAY (default and short enums)")


if __name__ == "__main__":
    main()
