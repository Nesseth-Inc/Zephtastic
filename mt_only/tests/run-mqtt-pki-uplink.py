#!/usr/bin/env python3
"""Offline PKI uplink regressions against the current ServiceEnvelope codec."""
import hashlib
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
APP = SOURCE / "mt_only"
PROTO = SOURCE / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"
def run(argv, **kwargs):
    print("$", " ".join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), check=True, **kwargs)


def main():
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="m122-pki-uplink-") as tmp:
        build = Path(tmp)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                  "-DPB_ENABLE_MALLOC", "-I" + str(HERE), "-I" + str(build),
                  "-I" + str(NANOPB), "-I" + str(APP / "src")]
        objects = []
        for source in (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c"):
            obj = build / (source.stem + ".o")
            run([*cc, "-std=c11", *common, "-Wall", "-Wextra", "-c", source, "-o", obj])
            objects.append(obj)
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            exe = build / ("m122-pki-uplink-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags, *common,
                 HERE / "mqtt_pki_uplink_tests.cpp", APP / "src/Protobuf.cpp", *objects, "-o", exe])
            result = subprocess.run([str(exe)], text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, env=env)
            print(label + ":\n" + result.stdout, end="", flush=True)
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
