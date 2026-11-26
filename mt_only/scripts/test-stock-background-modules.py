#!/usr/bin/env python3
"""Compare five production module families to executable functions from the pinned stock commit."""
from pathlib import Path
import argparse
import hashlib
import os
import shlex
import shutil
import subprocess

PIN = "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
FUNCTIONS = {
    "StatusMessage": ["int32_t StatusMessageModule::runOnce(", "ProcessMessage StatusMessageModule::handleReceived("],
    "RangeTest": ["void RangeTestModuleRadio::sendPayload(", "ProcessMessage RangeTestModuleRadio::handleReceived("],
    "NeighborInfo": ["bool NeighborInfoModule::handleReceivedProtobuf(", "void NeighborInfoModule::updateNeighbors(",
                     "NodeNum NeighborInfoModule::lastTransmitter("],
    "MeshBeacon": ["void MeshBeaconBroadcastModule::rebuildCache(", "void MeshBeaconBroadcastModule::sendBeacon()",
                   "int32_t MeshBeaconBroadcastModule::runOnce()", "bool MeshBeaconListenerModule::wantPacket("],
    "TrafficManagement": ["uint32_t secsToMs(", "uint8_t sanitizePositionPrecision(",
                          "uint8_t TrafficManagementModule::computePositionFingerprint(",
                          "bool TrafficManagementModule::isRateLimited(", "bool TrafficManagementModule::shouldDropUnknown("],
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    app = Path(__file__).resolve().parents[1]
    if out.exists() or out.is_relative_to(app.parent):
        parser.error("provide a fresh output directory outside the source checkout")
    out.mkdir(parents=True)
    proto = app.parent / "zephcore/bench/upstream-differential/source"
    headers = out / "meshtastic"
    headers.mkdir()
    for header in proto.glob("*.pb.h"):
        shutil.copyfile(header, headers / header.name)
    def read_stock(path, pin=PIN):
        return subprocess.check_output(["git", "-C", args.stock, "show", pin + ":" + path])
    for header in ["mesh.pb.h", "module_config.pb.h", "mesh_beacon.pb.h", "channel.pb.h"]:
        if (headers / header).read_bytes() != read_stock("src/mesh/generated/meshtastic/" + header, "d5f16191ca28b01b5024f590f04a67f25d66f1b1"):
            raise ValueError("descriptor differs from pinned donor: " + header)
    donor = []
    for module, functions in FUNCTIONS.items():
        path = f"src/modules/{module}Module.cpp"
        src = (subprocess.check_output(["git", "-C", args.stock, "show", "d5f16191ca28b01b5024f590f04a67f25d66f1b1:" + path]) if module == "NeighborInfo" else read_stock(path)).decode()
        for signature in functions:
            # Stock function bodies end with a brace in column zero.
            start = src.index("\n" + signature) + 1
            end = src.index("\n}", start) + 2
            donor.append(src[start:end])
        print(module, hashlib.sha256(src.encode()).hexdigest(), flush=True)
    (out / "stock_handlers.inc").write_text("\n\n".join(donor) + "\n")
    cc = shlex.split(os.environ.get("CC", "clang"))
    cxx = shlex.split(os.environ.get("CXX", "clang++"))
    for name, flags in [("normal", []), ("short", ["-fshort-enums"])]:
        run = out / name
        run.mkdir()
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-I" + str(out), "-I" + str(app / "src"),
                  "-I" + str(proto / "nanopb"), "-I" + str(app.parent / "zephcore/adapters/bench/vendor"),
                  "-I" + str(app.parent / "zephcore/lib/monocypher")]
        inputs = [proto / "nanopb" / n for n in ["pb_common.c", "pb_decode.c", "pb_encode.c"]]
        inputs += [app / "src/Protobuf.cpp", app / "tests/stock_background_modules.cpp"]
        objects = []
        for src in inputs:
            compiler = cc + ["-std=c11"] if src.suffix == ".c" else cxx + ["-std=c++17"]
            obj = run / (src.stem + ".o")
            subprocess.run([*compiler, *flags, "-c", src, "-o", obj], check=True)
            objects.append(obj)
        binary = run / "compare"
        subprocess.run([*cxx, "-fsanitize=address,undefined", *objects, "-o", binary], check=True)
        result = subprocess.check_output([binary], text=True)
        (run / "result.txt").write_text(result)
        print(name, result, end="", flush=True)

if __name__ == "__main__":
    main()
