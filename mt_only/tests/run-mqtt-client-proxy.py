#!/usr/bin/env python3
"""Build/run the bounded stock MQTT client-proxy codec and one-slot bridge tests."""
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "mt_only"
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"


def run(command):
    subprocess.run(list(map(str, command)), check=True)


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-client-proxy-") as temporary:
        build = Path(temporary)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                  "-DPB_ENABLE_MALLOC", "-I" + str(build), "-I" + str(NANOPB),
                  "-I" + str(APP / "src")]
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            objects = []
            for source in (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c"):
                obj = build / (label + "-" + source.stem + ".o")
                run([*cc, "-std=c11", *common, *enum_flags, "-Wall", "-Wextra",
                     "-c", source, "-o", obj])
                objects.append(obj)
            executable = build / ("mqtt-client-proxy-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags,
                 *common, APP / "tests/mqtt_client_proxy_tests.cpp",
                 APP / "src/Protobuf.cpp", *objects, "-o", executable])
            subprocess.run([str(executable)], check=True, env=env)
    print("MQTT client-proxy codec/queue tests PASS (default and short enums)")


if __name__ == "__main__":
    main()
