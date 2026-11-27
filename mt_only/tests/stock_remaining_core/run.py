#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare beacon receive and transmit decisions and evaluate target-specific registrations."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
ROOT = APP.parent
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

STOCK = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
PINS = {
    "MeshBeaconModule.cpp": "c7e825667b18792af2ff8a7a58e33f399fc41834c0412d5b6d45fcfe35b7b470",
    "MeshBeaconModule.h": "8f32ade6824e5754ef245fbafa49baf2f8b8061776cd9f402487a1d18701a4fd",
    "Modules.cpp": "597eb62b9cd15e8fa44341fee39d725b72a94e92996c97883c499fc9cc534da7",
}


def registration_profiles(source, work):
    first = source.index("#if ARCH_PORTDUINO\n    new HostMetricsModule();")
    last = source.index("#if HAS_TELEMETRY", first)
    guard = work / "profile.cpp"
    guard.write_text(source[first:last])
    checked = []
    for linux, shell, peer in itertools.product((False, True), repeat=3):
        defines = [f"-DARCH_PORTDUINO={int(linux)}"]
        if shell:
            defines.append("-DMESHTASTIC_HAS_DMSHELL=1")
        if peer:
            defines.append("-DDMSHELL_TEST_PEER=1")
        active = subprocess.check_output([*host.cxx(), "-E", "-P", *defines, guard], text=True)
        if ("new DMShellModule()" in active) != (linux and shell) or ("new DMShellTestModule()" in active) != peer:
            raise ValueError("Unexpected stock registration for the selected profile")
        checked.append({"portduino": linux, "shell": shell, "test_peer": peer})
    return checked


def sensor_inventory(stock):
    directory = "src/modules/Telemetry/Sensor"
    base = "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
    def objects(revision):
        lines = subprocess.check_output(["git", "-C", stock, "ls-tree", "-r", revision, directory], text=True)
        return {path: meta.split()[2] for meta, path in (line.split("\t") for line in lines.splitlines())}
    old, new = objects(base), objects(STOCK)
    additions = {"RAKSensorHub.cpp", "RAKSensorHub.h", "RAKSensorHubUplink.cpp"}
    if not old.items() <= new.items() or {Path(p).name for p in new.keys() - old.keys()} != additions:
        raise ValueError("A stock sensor backend changed outside the reviewed RAK hub additions")
    unchanged = sorted(Path(p).stem for p in old if p.endswith(".cpp"))
    if len(unchanged) != 46:
        raise ValueError("Unexpected telemetry sensor inventory")
    for name in ("RAKSensorHub.cpp", "RAKSensorHubUplink.cpp"):
        source = (stock / directory / name).read_text()
        if "#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR && defined(HAS_RAKHUB)" not in source:
            raise ValueError(f"The RAK2560-only guard changed in {name}")
    return {"unchanged_translation_units": unchanged, "rak2560_additions": sorted(additions)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--maxim", required=True, type=Path)
    args = parser.parse_args()
    sensors = sensor_inventory(args.stock)
    sources = {}
    for name, digest in PINS.items():
        data = (args.stock / "src/modules" / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Unexpected upstream source {name}")
        sources[name] = data.decode()
    extract = runpy.run_path(str(ROOT / "zephcore/bench/run-routing-differential.py"))["extract"]
    proto = ROOT / "zephcore/bench/upstream-differential/source"
    with tempfile.TemporaryDirectory(prefix="zt-beacon-hooks-") as tmp:
        work = Path(tmp)
        profiles = registration_profiles(sources["Modules.cpp"], work)
        (work / "meshtastic").symlink_to(proto, target_is_directory=True)
        header = sources["MeshBeaconModule.h"]
        start = header.index("    struct BeaconOffer {")
        (work / "offer.inc").write_text(header[start:header.index("    };", start) + 6] + "\n")
        flag = next(line for line in header.splitlines() if line.startswith("#define MESH_BEACON_FLAG_LISTEN_ENABLED "))
        (work / "listen-flag.inc").write_text(flag + "\n")
        methods = ["bool MeshBeaconListenerModule::wantPacket(", "bool MeshBeaconListenerModule::handleReceivedProtobuf(",
                   "RadioTxHook::PreTxAction MeshBeaconTxHook::beforeTransmit(", "bool MeshBeaconTxHook::holdsRadio(",
                   "void MeshBeaconTxHook::packetReleased("]
        (work / "methods.inc").write_text("\n\n".join(extract(sources["MeshBeaconModule.cpp"], m)[0] for m in methods))
        counts = [0, 0]
        for flags in ([], ["-fshort-enums"]):
            exe = work / "compare"
            includes = (work, APP / "src", APP / "tests/beacon/stubs", ROOT / "zephcore/adapters/bench/vendor",
                        ROOT / "zephcore/lib/monocypher", proto / "nanopb")
            subprocess.run([*host.cxx(), "-std=c++17", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            *flags, *(f"-I{p}" for p in includes), HERE / "beacon.cpp",
                            APP / "src/modules/BeaconExcursion.cpp", "-o", exe], check=True)
            results = list(map(int, subprocess.check_output([exe], text=True).split()))
            if results != [256, 3]:
                raise ValueError(f"Incomplete listener or hook scenarios {results}")
            counts = [a+b for a, b in zip(counts, results)]
        runtime_report = work / "beacon-target.json"
        subprocess.run([sys.executable, APP / "tests/stock_beacon_tx/run.py", "--stock", args.stock,
                        "--output", runtime_report], check=True)
        target_runtime = json.loads(runtime_report.read_text())
        regression = work / "received"
        try:
            subprocess.run([sys.executable, APP / "scripts/test-extended-modules.py", "--output", regression,
                            "--only", "mesh_beacon_received_cache_tests", "beacon_controller_tests"], check=True)
        except subprocess.CalledProcessError:
            logs = sorted(regression.rglob("*.log"), key=lambda path: path.stat().st_mtime)
            for path in logs[-2:]:
                print("\n".join(path.read_text(errors="replace").splitlines()[-12:]), file=sys.stderr)
            raise
        native = json.loads((regression / "RESULT.json").read_text())
        if native["status"] != "PASS" or len(native["executions"]) != 4:
            raise ValueError("Incomplete receive-cache and controller regressions")
    subprocess.run([sys.executable, APP / "tests/stock_sensor_channels/run.py", "--release", "2.8.2",
                    "--stock", args.stock, "--maxim", args.maxim], check=True)
    sensors["execution"] = "Zephyr sensor workers, stock conversion bodies, two enum widths, and pinned Maxim optical sample processing"
    rows = {
        "MeshBeaconListenerModule": {"kind": "differential", "comparisons": counts[0],
            "evidence": "Stock listen admission and retained offer fields compared across ports and offer combinations. ZT receive-service and advisory-cache regressions also ran."},
        "MeshBeaconTxHook": {"kind": "differential", "comparisons": counts[1]+sum(r["runtime_scenarios"] for r in target_runtime["runs"]),
            "runtime": target_runtime,
            "evidence": "The production target owner deferred after retune, repeated contention on busy CAD, and preserved target hashes, complete frames, FIFO order and live home settings against pinned hook/timing functions and stock regression cases. CAD and TX completion used a synthetic radio."},
        "DMShellModule": {"kind": "profile", "evidence": "Stock registers this owner only in Portduino builds with the shell enabled. Shipping ZT boards use Zephyr."},
        "DMShellTestModule": {"kind": "profile", "evidence": "Stock registers this owner only in a designated test-peer build."},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"stock_revision": STOCK, "modules": rows, "source_sha256": PINS,
                                      "registration_profiles": profiles, "sensor_backends": sensors, "hardware": False}, indent=2) + "\n")


if __name__ == "__main__":
    main()
