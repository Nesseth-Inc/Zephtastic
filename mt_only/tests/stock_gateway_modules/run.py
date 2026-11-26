#!/usr/bin/env python3
"""Check portable gateway and mesh policies against the pinned Meshtastic translation units."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

PIN = "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PROTO = ROOT / "zephcore/bench/upstream-differential/source"


def function(source, start):
    begin = source.index(start)
    return source[begin:source.index("\n}", source.index("\n{", begin)) + 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", required=True, type=Path)
    args = parser.parse_args()

    def donor(path):
        return subprocess.check_output(["git", "-C", args.stock, "show", f"{PIN}:src/{path}"], text=True)

    release_pin = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
    def release(path):
        return subprocess.check_output(["git", "-C", args.stock, "show", f"{release_pin}:src/{path}"], text=True)
    identity = function(release("mesh/NodeDB.cpp"), "bool coerceNodeInfoUserId(")
    opaque = function(release("mesh/Router.cpp"), "void Router::perhapsUplinkOpaquePki(")
    mqtt = donor("mqtt/MQTT.cpp")
    report = function(mqtt, "void MQTT::perhapsReportToMap(")
    map_precision = report[report.index("    if (map_position_precision <"):report.index("    if (Throttle::")]
    fields = report[report.index("    // Fill MapReport message"):report.index("    // Encode MapReport message")]
    oracle = "\n".join((function(mqtt, "void MQTT::sendSubscriptions("),
                        function(mqtt, "void MQTT::onClientProxyReceive("),
                        "meshtastic_MapReport fillMapReport(uint32_t map_position_precision) {",
                        map_precision, fields, "return mapReport; }"))
    traffic = donor("modules/TrafficManagementModule.cpp")
    neighbor = release("modules/NeighborInfoModule.cpp")
    mesh_oracle = "\n".join([function(traffic, "TrafficManagementModule::DirectResponseThrottleEntry *\nTrafficManagementModule::directResponseSlot("),
                            function(traffic, "bool TrafficManagementModule::directResponseAllowed("),
                            function(neighbor, "meshtastic_Neighbor *NeighborInfoModule::getOrCreateNeighbor("),
                            function(neighbor, "void NeighborInfoModule::cleanUpNeighbors("),
                            function(neighbor, "uint32_t NeighborInfoModule::collectNeighborInfo("),
                            function(neighbor, "int32_t NeighborInfoModule::runOnce(")])
    with tempfile.TemporaryDirectory(prefix="zt-stock-gateway-") as directory:
        build = Path(directory)
        inc = build / "meshtastic"
        inc.mkdir()
        for header in PROTO.glob("*.pb.h"):
            (inc / header.name).symlink_to(header)
        (build / "identity-oracle.inc").write_text(identity + "\n" + opaque)
        (build / "oracle.inc").write_text(oracle)
        (build / "mesh-oracle.inc").write_text(mesh_oracle)
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        cc = shlex.split(os.environ.get("CC", "cc"))
        for abi in ([], ["-fshort-enums"]):
            objects = []
            for name in ("pb_common", "pb_encode", "pb_decode"):
                obj = build / (name + ".o")
                subprocess.run([*cc, "-std=c11", "-O1", "-fsanitize=address,undefined", *abi, "-c",
                                PROTO / "nanopb" / (name + ".c"), "-o", obj], check=True)
                objects.append(obj)
            exe = build / "gateway-tests"
            subprocess.run([*compiler, "-std=c++17", "-O1", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", *abi, "-I", build, "-I", PROTO / "nanopb",
                            "-I", ROOT / "mt_only/src", "-I", ROOT / "zephcore/adapters/bench/vendor",
                            "-I", ROOT / "zephcore/lib/monocypher", HERE / "cases.cpp", HERE / "mesh_cases.cpp",
                            HERE / "identity_cases.cpp", HERE / "responder_cases.cpp", ROOT / "mt_only/src/modules/TrafficNodeInfoResponder.cpp",
                            ROOT / "mt_only/src/Protobuf.cpp", *objects, "-o", exe], check=True)
            subprocess.run([exe], check=True, env={**os.environ, "ASAN_OPTIONS":"detect_leaks=0",
                                                   "UBSAN_OPTIONS":"halt_on_error=1"})
            print("PASS", "short-enums" if abi else "default-enums", flush=True)


if __name__ == "__main__":
    main()
