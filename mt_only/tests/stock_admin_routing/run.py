#!/usr/bin/env python3
"""Compare firmware dispatch and routing policy with original pinned methods."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

PIN = "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
PROTO = APP.parent / "zephcore/bench/upstream-differential/source"

def method(source, name):
    start = source.index(name)
    return source[start:source.index("\n}", start) + 2]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stock", required=True, type=Path)
args = parser.parse_args()

def donor(path):
    return subprocess.check_output(["git", "-C", args.stock, "show", f"{PIN}:src/modules/{path}.cpp"], text=True)

routing = donor("RoutingModule")
parts = [method(donor("AdminModule"), "static pb_size_t adminResponseForRequest("),
         method(routing, "bool RoutingModule::handleReceivedProtobuf("),
         method(routing, "uint8_t RoutingModule::getHopLimitForResponse(")]
with tempfile.TemporaryDirectory(prefix="zt-admin-routing-") as td:
    path = Path(td)
    (path / "meshtastic").mkdir()
    for header in PROTO.glob("*.pb.h"):
        (path / "meshtastic" / header.name).symlink_to(header)
    (path / "stock.inc").write_text("\n".join(parts))
    (path / "stock_pins.inc").write_text(method(donor("AdminModule"), "void AdminModule::handleGetNodeRemoteHardwarePins("))
    objects = []
    for source in ["pb_common.c", "pb_encode.c"]:
        obj = path / (source + ".o")
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-I", PROTO / "nanopb",
                        "-c", PROTO / "nanopb" / source, "-o", obj], check=True)
        objects.append(obj)
    exe = path / "wire-pins"
    subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-O1", "-I", path,
                    "-I", APP / "src", "-I", PROTO / "nanopb", HERE / "pins.cpp", *objects, "-o", exe], check=True)
    subprocess.run([exe], check=True)
    for mode in ([], ["-fshort-enums"]):
        exe = path / "compare"
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-O1",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *mode,
                        "-I", path, "-I", PROTO / "nanopb", "-I", APP / "src",
                        HERE / "cases.cpp", "-o", exe], check=True)
        subprocess.run([exe], check=True, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
    exe = path / "app-file"
    subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-O1",
                    "-fsanitize=address,undefined", "-DCONFIG_MT_RANGE_TEST_LOG=1", "-DCONFIG_FILE_SYSTEM=1",
                    '-DCONFIG_MT_RANGE_TEST_LOG_DIR="/mounted"', "-I", HERE / "fs", "-I", APP / "src",
                    HERE / "fs.cpp", APP / "src/modules/RangeTestLog.cpp", "-o", exe], check=True)
    subprocess.run([exe], check=True)
    exe = path / "factory-config-reset"
    subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-O1",
                    "-fsanitize=address,undefined", "-I", APP / "src", HERE / "factory.cpp", "-o", exe], check=True)
    subprocess.run([exe], check=True)
