#!/usr/bin/env python3
"""Differentially encode the catalogue against the pinned stock producer."""
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "mt_only"
DONOR = ROOT / "zephcore/bench/upstream-differential/source"
RADIO_CPP = DONOR / "RadioInterface.cpp"
PROTO_H = DONOR / "mesh.pb.h"
PROTOLOCK = DONOR / "source-lock.json"
PROTOBIND = APP / "src/Protobuf.cpp"
HEADER = APP / "src/RegionCatalogue.h"
POLICY = APP / "src/RegionPolicy.h"
FIXTURE = APP / "tests/region_catalogue_cases.cpp"
RADIO_SHA256 = "0de8434b14c3c1deb383b4510b96a9d0d750a797e9140f9cfb08147e4a749506"
MESH_SHA256 = "4176759182979ec9547b6abd5e0bd7e425e1d155d92345fa086184e81c2986ab"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_function(source):
    signature = "void getRegionPresetMap(meshtastic_LoRaRegionPresetMap &map)"
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    i = opening + 1
    while depth:
        depth += (source[i] == "{") - (source[i] == "}")
        i += 1
    return source[start:i]


def run_checked(command, env=None, capture=False):
    print("$", " ".join(map(str, command)), flush=True)
    return subprocess.run(command, check=True, env=env, text=True,
                          capture_output=capture)


def main():
    assert digest(RADIO_CPP) == RADIO_SHA256, "pinned RadioInterface.cpp changed"
    assert digest(PROTO_H) == MESH_SHA256, "pinned mesh.pb.h changed"
    source_pins = [RADIO_CPP, PROTO_H, PROTOLOCK, PROTOBIND, HEADER, POLICY, FIXTURE]
    before = {str(path): digest(path) for path in source_pins}
    stock_body = extract_function(RADIO_CPP.read_text())
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    cc = shlex.split(os.environ.get("CC", "cc"))
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"

    wire_hashes = {}
    with tempfile.TemporaryDirectory(prefix="mt-region-catalogue-") as tmp:
        out = Path(tmp)
        generated = out / "meshtastic"
        generated.mkdir()
        for header in DONOR.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        (out / "stock_catalogue_producer.inc").write_text(stock_body + "\n")
        c_objects = []
        for name in ("pb_common.c", "pb_encode.c", "pb_decode.c"):
            obj = out / (name + ".o")
            run_checked([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                         "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                         "-g", "-O1", "-I" + str(DONOR / "nanopb"),
                         "-c", str(DONOR / "nanopb" / name), "-o", str(obj)])
            c_objects.append(obj)

        for label, enum_flags in (("default-enum", []),
                                  ("short-enum", ["-fshort-enums"])):
            exe = out / ("region-catalogue-" + label)
            command = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       # Upstream's exact RegionInfo preset sentinel is an
                       # out-of-enumerator enum value 0xff; its comparison is
                       # deliberate stock code. Keep other UBSan checks on.
                       "-fno-sanitize=enum", "-g", "-O1", *enum_flags,
                       "-I" + str(APP / "src"), "-I" + str(out),
                       "-I" + str(DONOR / "nanopb"), str(FIXTURE), str(PROTOBIND),
                       *(str(obj) for obj in c_objects), "-o", str(exe)]
            run_checked(command)
            try:
                result = run_checked([str(exe)], env=env, capture=True)
            except subprocess.CalledProcessError as error:
                if error.stdout:
                    print(error.stdout, end="", flush=True)
                if error.stderr:
                    print(error.stderr, end="", flush=True)
                raise
            combined = result.stdout + result.stderr
            match = re.search(r"^WIREHEX:([0-9a-f]+)$", combined, re.MULTILINE)
            assert match, f"wire hex was not emitted in {label} run"
            wire = bytes.fromhex(match.group(1))
            wire_hash = hashlib.sha256(wire).hexdigest()
            wire_hashes[label] = wire_hash
            print(combined.replace(match.group(0) + "\n", ""), end="", flush=True)
            print(f"PASS {label} wire bytes={len(wire)} sha256={wire_hash}", flush=True)

    assert wire_hashes["default-enum"] == wire_hashes["short-enum"], \
        "short-enum mode changed protobuf wire bytes"
    after = {str(path): digest(path) for path in source_pins}
    assert after == before, "pinned source changed during native run"
    print("PASS pinned stock producer differential; source pins unchanged", flush=True)


if __name__ == "__main__":
    main()
