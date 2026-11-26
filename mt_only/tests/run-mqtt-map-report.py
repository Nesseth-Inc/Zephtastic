#!/usr/bin/env python3
"""Build/run the bounded MqttMapReporter fixture under ASan/UBSan."""
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


def run(command, env=None):
    print("$", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, env=env)


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-map-report-") as tmp:
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
        event_flags = ["-DUSERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL=1",
                       "-DUSERPREFS_CHANNEL_0_PSK={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}"]
        for label, enum_flags, policy_flags in (
                ("default-enums", [], []),
                ("short-enums", ["-fshort-enums"], []),
                ("event-key-default-enums", [], event_flags),
                ("event-key-short-enums", ["-fshort-enums"], event_flags)):
            objects = []
            for source in (NANOPB / "pb_common.c", NANOPB / "pb_encode.c",
                           NANOPB / "pb_decode.c"):
                obj = build / (label + "-" + source.stem + ".o")
                run([*cc, "-std=c11", *common, *enum_flags, "-Wall", "-Wextra",
                     "-c", source, "-o", obj])
                objects.append(obj)
            exe = build / ("mqtt-map-report-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags, *policy_flags,
                 *common, APP / "tests/mqtt_map_report_tests.cpp",
                 APP / "src/Protobuf.cpp", *objects, "-o", exe])
            result = subprocess.run([str(exe)], check=False, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    env=env)
            print(label + ":", flush=True)
            print(result.stdout, end="")
            if result.returncode != 0:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
