#!/usr/bin/env python3
"""Stock Python MeshInterface and Node API against memory-backed ZT transport endpoints."""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
from unittest.mock import patch

APP = Path(__file__).resolve().parents[2]
ROOT = APP.parent
sys.path[:0] = [str(ROOT), str(APP / "tests/stock-private-client")]
import host_test_toolchain as host
from client import StockClient, mesh_interface, mesh_pb2, portnums_pb2, provenance
from meshtastic.protobuf import admin_pb2

NODE = 0x12345678
FULL_DUMP, CONFIG_ONLY, NODES_ONLY = 71, 69420, 69421


def build_fixture(out):
    proto = ROOT / "zephcore/bench/upstream-differential/source"
    vendor = ROOT / "zephcore/adapters/bench/vendor"
    mono = ROOT / "zephcore/lib/monocypher"
    generated = out / "meshtastic"
    generated.mkdir()
    for header in proto.glob("*.pb.h"):
        shutil.copyfile(header, generated / header.name)
    flags = ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", APP / "src",
             "-I", vendor, "-I", mono, "-I", out, "-I", proto / "nanopb", *host.crypto_compile_flags()]
    objects = []
    for src in [mono / "monocypher.c", mono / "monocypher-ed25519.c",
                *(proto / "nanopb" / f for f in ("pb_common.c", "pb_encode.c", "pb_decode.c"))]:
        obj = out / (src.stem + ".o")
        subprocess.run([*host.cc(), "-std=c11", *flags, "-c", src, "-o", obj], check=True)
        objects.append(obj)
    exe = out / "device-config-oracle"
    subprocess.run([*host.cxx(), "-std=c++17", *flags, APP / "tests/device_config_oracle.cpp",
                    APP / "src/Protobuf.cpp", vendor / "MtCodec.cpp", vendor / "MtSignature.cpp",
                    APP / "src/pki/PkiSigner.cpp", APP / "src/pki/PkiIdentity.cpp", *objects,
                    *host.crypto_link_flags(), "-o", exe], check=True)
    return exe


def session(exe, model, transport, out):
    checks = []
    def require(label, value):
        if not value:
            raise AssertionError(f"{model}/{transport}: {label}")
        checks.append(label)

    with (out / f"{model}-{transport}.stderr").open("w") as stderr:
        with subprocess.Popen([exe, str(model)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                              text=True, bufsize=1, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0",
                                                        "UBSAN_OPTIONS": "halt_on_error=1"}) as fixture:
            def exchange(command):
                fixture.stdin.write(command + "\n")
                fixture.stdin.flush()
                if not select.select([fixture.stdout], [], [], 10)[0]:
                    raise TimeoutError("The native companion stopped responding")
                line = fixture.stdout.readline()
                if not line:
                    raise RuntimeError(f"Native companion exited. See {stderr.name}")
                row = json.loads(line)
                require("radio stayed idle", not (row["radio_sends"] or row["radio_starts"] or row["tx_armed"]))
                return [mesh_pb2.FromRadio.FromString(bytes.fromhex(raw)) for raw in row["records_hex"]], row

            def snapshot(nonce):
                actual = StockClient(NODE, 0x87654321)
                with patch.object(mesh_interface.random, "randint", return_value=nonce):
                    actual._startConfig()
                wire = actual.sent[-1]
                require("stock API requested expected config nonce", mesh_pb2.ToRadio.FromString(wire).want_config_id == nonce)
                records, row = exchange(f"{transport} {wire.hex()}")
                actual.accept(row["records_hex"])
                complete = [i for i,r in enumerate(records) if r.HasField("config_complete_id")]
                require("single completion with echoed nonce", len(complete) == 1 and records[complete[0]].config_complete_id == nonce)
                # FromRadio events after config_complete_id are asynchronous replay.
                dump = records[:complete[0]]
                local_nodes = [r.node_info for r in dump if r.HasField("node_info")]
                require("local node and board model", len(local_nodes) == 1 and local_nodes[0].num == NODE and
                        local_nodes[0].user.hw_model == model)
                if nonce == NODES_ONLY:
                    require("nodes-only excludes all other dump sections", len(dump) == len(local_nodes))
                else:
                    require("local identity", dump[0].HasField("my_info") and dump[0].my_info.my_node_num == NODE)
                    require("metadata board model", [r.metadata.hw_model for r in dump if r.HasField("metadata")] == [model])
                    require("ordered channel indices", [r.channel.index for r in dump if r.HasField("channel")] == list(range(8)))
                    require("device config and module config", any(r.HasField("config") and r.config.HasField("device") for r in dump)
                            and any(r.HasField("moduleConfig") for r in dump))
                return actual

            try:
                client = snapshot(FULL_DUMP)
                require("initial stock Device config", client.localNode.localConfig.device.node_info_broadcast_secs == 10800)
                snapshot(CONFIG_ONLY)
                snapshot(NODES_ONLY)
                # Start a full epoch again before calling the actual Node API.
                client = snapshot(FULL_DUMP)
                client.localNode.localConfig.device.node_info_broadcast_secs = 7200
                count = len(client.sent)
                client.localNode.writeConfig("device")
                writes = client.sent[count:]
                require("actual Node.writeConfig emitted a request", 1 <= len(writes) <= 2)
                for wire in writes:
                    request = mesh_pb2.ToRadio.FromString(wire).packet
                    require("local ADMIN_APP", request.to == NODE and request.decoded.portnum == portnums_pb2.ADMIN_APP)
                    records, row = exchange(f"{transport} {wire.hex()}")
                    client.accept(row["records_hex"])
                    admin = admin_pb2.AdminMessage.FromString(request.decoded.payload)
                    if admin.HasField("set_config"):
                        replies = [r.packet for r in records if r.HasField("packet") and r.packet.decoded.request_id == request.id]
                        require("correlated successful routing ACK", len(replies) == 1 and
                                replies[0].decoded.portnum == portnums_pb2.ROUTING_APP and
                                mesh_pb2.Routing.FromString(replies[0].decoded.payload).error_reason == mesh_pb2.Routing.NONE)
                _, saved = exchange("restart")
                require("persisted setting on fresh runtime", saved["interval"] == 7200)
                client = snapshot(FULL_DUMP)
                require("stock client decoded persisted setting", client.localNode.localConfig.device.node_info_broadcast_secs == 7200)
            finally:
                fixture.stdin.close()
                try:
                    fixture.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    fixture.kill()
                    fixture.wait()
            require("native companion exited cleanly", fixture.returncode == 0)
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    version = provenance()
    exe = build_fixture(out)
    results = {f"{model}/{transport}": session(exe, model, transport, out)
               for model in (81, 127) for transport in ("usb", "ble")}
    (out / "results.json").write_text(json.dumps({"client": version, "hardware": False, "checks": results}, indent=2) + "\n")
    print(f"PASS stock Python handshake, config selection, Device save and journal restart in {len(results)} model/transport combinations")


if __name__ == "__main__":
    main()
