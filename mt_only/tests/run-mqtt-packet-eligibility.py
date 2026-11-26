#!/usr/bin/env python3
"""Build/run the MQTT eligibility policy fixture with default and short enums."""
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
VENDOR = ROOT / "zephcore/adapters/bench/vendor"
MONO = ROOT / "zephcore/lib/monocypher"


def run(command):
    print("$", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True)


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-eligibility-") as tmp:
        build = Path(tmp)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                  "-I" + str(build), "-I" + str(NANOPB), "-I" + str(APP / "src"),
                  "-I" + str(APP / "tests"), "-I" + str(VENDOR), "-I" + str(MONO)]
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            objects = []
            for source in (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c"):
                obj = build / (label + "-" + source.stem + ".o")
                run([*cc, "-std=c11", *common, *enum_flags, "-Wall", "-Wextra", "-c", source, "-o", obj])
                objects.append(obj)
            exe = build / ("mqtt-eligibility-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags, *common,
                 APP / "tests/mqtt_packet_eligibility_tests.cpp", APP / "src/Protobuf.cpp",
                 *objects, "-o", exe])
            result = subprocess.run([str(exe)], check=False, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, env=env)
            print(label + ":", flush=True)
            print(result.stdout, end="")
            if result.returncode != 0:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
