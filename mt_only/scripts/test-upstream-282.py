#!/usr/bin/env python3
"""Run pinned Meshtastic 2.8.2 Unity tests and a stock Python client against ZT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "mt_only"
FIXTURE = APP / "tests/upstream_282"
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

FIRMWARE = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
UNITY = "cbcd08fa7de711053a3deec6339ee89cad5d2697"
BME = "test/test_bme680_iaq/test_main.cpp"
HOPS = "test/test_routing_response_hops/test_main.cpp"
REBROADCAST = "test/test_rebroadcast_mode/test_main.cpp"
AUTH_HARNESS = "test/support/AuthPipelineHarness.h"
PINS = {
    REBROADCAST: "f7b231458bed04ac4097c5f5ed4c116caf687af91f612d3e72ae267e59bb615f",
    "src/mesh/FloodingRouter.cpp": "37fdf91c01e2f574f72ac12ab2443ba5fc248fae5f644906abff8df1197f4c9d",
    AUTH_HARNESS: "0eb9d263fd60bdc28e47a9f0393ce136c55cfb8f9dd20644bfe04e2475f471c7",
    BME: "a52582ad86703e522b6fe0f6326ef84813d0630860f40c2280ac187097ab6784",
    HOPS: "2114aae0ea1ed25d6ab8b51ab0cb3b6f499ecbc1f8aac538b0b44ce319c47939",
    "src/mesh/NodeDB.cpp": "006921660b5b4b7e8ee32fecc4dcb84cde3e31d0c0853c335e0c43b02f31d3e1",
    "src/mesh/Default.cpp": "a0a46f1b5a6173fc0365f4cb50353fb52ae6748ee2e036ddec4a7900918f9443",
    "src/modules/RoutingModule.cpp": "82d54c9dc9e2c81f2f427d27c8c2dbc9a8409739b030f7e48db95901e21e8031",
}
UNITY_PINS = {
    "unity.c": "b90e735a54cf3b3765ab6caa955d11a1488ee73d9c6152cdc98576c2d17cb871",
    "unity.h": "9db174d3c2c6424fd35c0980c5941d124c5ebb0f48e8172f997a2aa9554b64ea",
    "unity_internals.h": "fcd8b3f6b412ac0ab599547eb8a30b6d7f3f0af77aab31f7a1822a2a8fc9a2b2",
}
extract = runpy.run_path(str(ROOT / "zephcore/bench/run-routing-differential.py"))["extract"]


def verified(data, digest, name):
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"{name} does not match its pinned source")
    return data


def tests_in(source, expected):
    names = re.findall(r"^\s*RUN_TEST\((test_\w+)\);", source, re.MULTILINE)
    if len(names) != expected or len(set(names)) != expected:
        raise ValueError(f"Expected {expected} distinct upstream tests, found {names}")
    return names


def run_unity(executable, names, output):
    result = subprocess.run([executable], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=60, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
    print(result.stdout, end="", flush=True)
    (output / (executable.name + ".log")).write_text(result.stdout)
    result.check_returncode()
    passes = re.findall(r":(test_\w+):PASS\s*$", result.stdout, re.MULTILINE)
    if passes != names or f"{len(names)} Tests 0 Failures 0 Ignored" not in result.stdout:
        raise RuntimeError(f"{executable.name} did not run all selected upstream assertions")
    return passes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unity", type=Path, help="Optional local Unity src directory")
    args = parser.parse_args()
    stock = args.stock.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = {name: verified((stock / name).read_bytes(), digest, name).decode()
               for name, digest in PINS.items()}

    unity = out / "unity"
    unity.mkdir()
    for name, digest in UNITY_PINS.items():
        if args.unity:
            data = (args.unity / name).read_bytes()
        else:
            url = f"https://raw.githubusercontent.com/ThrowTheSwitch/Unity/{UNITY}/src/{name}"
            with urlopen(url, timeout=30) as response:
                data = response.read()
        (unity / name).write_bytes(verified(data, digest, name))
    flags = ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", out, "-I", unity, "-I", APP / "src"]
    obj = out / "unity.o"
    subprocess.run([*host.cc(), "-std=c11", *flags, "-c", unity / "unity.c", "-o", obj], check=True)
    cxx = [*host.cxx(), "-std=c++17", *flags]
    results = {}

    # Preserve the entire BME680 suite. Only the Arduino setup hook and header
    # locations change; every assertion runs against the estimator shipped in ZT.
    (out / "MeshTypes.h").write_text("#pragma once\n")
    (out / "TestUtil.h").write_text("#pragma once\n#include <cstdlib>\ninline void initializeTestEnvironment() {}\n")
    header = out / "modules/Telemetry/Sensor/BME680IaqEstimator.h"
    header.parent.mkdir(parents=True)
    header.write_text('#include "stock/telemetry/bme680/BME680IaqEstimator.h"\n')
    (out / "unity-main.cpp").write_text("void setup();\nint main() { setup(); return 1; }\n")
    exe = out / "bme680-zt"
    subprocess.run([*cxx, stock / BME, APP / "src/stock/telemetry/bme680/BME680IaqEstimator.cpp",
                    out / "unity-main.cpp", obj, "-o", exe], check=True)
    results[exe.name] = run_unity(exe, tests_in(sources[BME], 15), out)

    # The other two upstream cases exercise MeshModule::setReplyTo. ZT has no
    # corresponding generic entry point. Keep the exclusion and total explicit.
    excluded = {"test_setreplyto_stamps_computed_hop_limit_and_reply_fields",
                "test_setreplyto_preserves_existing_priority"}
    all_hops = tests_in(sources[HOPS], 16)
    if not excluded.issubset(all_hops):
        raise RuntimeError("The upstream response-hop test selection changed")
    names = [name for name in all_hops if name not in excluded]
    # In event mode upstream has a second, empty Unity runner with its own
    # setUp/tearDown. Extract from the ordinary build's active region only.
    standard, guard, _ = sources[HOPS].partition("#else // USERPREFS_EVENT_MODE")
    if not guard or standard.count("#if !USERPREFS_EVENT_MODE") != 1:
        raise ValueError("Missing upstream standard-mode test guard")
    bodies = [extract(standard, "static meshtastic_MeshPacket makeRequest(")[0],
              extract(standard, "void setUp(")[0], extract(standard, "void tearDown(")[0]]
    bodies.extend(extract(standard, f"void {name}(")[0] for name in names)
    bodies += ["int main() {", "    UNITY_BEGIN();", *(f"    RUN_TEST({name});" for name in names),
               "    return UNITY_END();", "}"]
    (out / "stock-hop-tests.inc").write_text("\n".join(bodies) + "\n")
    methods = (("src/mesh/NodeDB.cpp", "int8_t getHopsAway("),
               ("src/mesh/Default.cpp", "uint8_t Default::getConfiguredOrDefaultHopLimit("),
               ("src/modules/RoutingModule.cpp", "uint8_t RoutingModule::getHopLimitForResponse("))
    (out / "stock-implementation.inc").write_text(
        "\n\n".join(extract(sources[name], marker)[0] for name, marker in methods) + "\n")
    for label, mode in (("reply-hops-stock", 1), ("reply-hops-zt", 0)):
        exe = out / label
        subprocess.run([*cxx, f"-DSTOCK_ORACLE={mode}", FIXTURE / "reply_hops.cpp", obj, "-o", exe], check=True)
        results[label] = run_unity(exe, names, out)

    rebroadcast_cases = ["test_unknown_channel_broadcast_relays_in_core_portnums_only",
                         "test_opaque_relay_header_gates_hold"]
    all_rebroadcast = tests_in(sources[REBROADCAST], 11)
    if not set(rebroadcast_cases).issubset(all_rebroadcast):
        raise RuntimeError("The upstream rebroadcast test selection changed")
    (out / "stock-rebroadcast.inc").write_text("\n\n".join((
        extract(sources[AUTH_HARNESS], "static const char *modeName(")[0],
        extract(sources[AUTH_HARNESS], "static meshtastic_MeshPacket makeUnknownChannelBroadcast(")[0],
        *(extract(sources[REBROADCAST], f"void {name}(")[0] for name in rebroadcast_cases),
    )) + "\n")
    flooding = sources["src/mesh/FloodingRouter.cpp"]
    (out / "stock-flooding.inc").write_text("\n\n".join(extract(flooding, signature)[0] for signature in (
        "bool FloodingRouter::isRebroadcaster(",
        "bool FloodingRouter::roleAllowsCancelingDupe(",
        "void FloodingRouter::perhapsCancelDupe(",
    )) + "\n")
    build_fixture = runpy.run_path(str(FIXTURE / "phone_config.py"))["build_fixture"]
    relay_out = out / "rebroadcast"
    relay_out.mkdir()
    relay = build_fixture(relay_out, FIXTURE / "rebroadcast.cpp", ["-I", out, "-I", unity], [obj])
    results["rebroadcast_zt"] = run_unity(relay, [*rebroadcast_cases, "test_zt_forwarding_and_duplicate_queue_against_stock"], relay_out)
    results["rebroadcast_modes"] = ["ALL", "CORE_PORTNUMS_ONLY", "KNOWN_ONLY", "LOCAL_ONLY", "NONE"]
    results["excluded_rebroadcast_mode"] = "ALL_SKIP_DECODING (deprecated stock REPEATER only)"
    results["other_upstream_rebroadcast_cases"] = [c for c in all_rebroadcast if c not in rebroadcast_cases]
    subprocess.run([sys.executable, APP / "scripts/test-endpoint-differential.py", "--stock", stock,
                    "--output", out / "endpoint"], check=True)
    subprocess.run([sys.executable, FIXTURE / "phone_config.py", "--output", out / "phone"], check=True)
    results["endpoint"] = json.loads((out / "endpoint/results.json").read_text())
    results["phone_config"] = json.loads((out / "phone/results.json").read_text())
    (out / "results.json").write_text(json.dumps({
        "stock_revision": FIRMWARE, "unity_revision": UNITY, "source_sha256": PINS,
        "unity_sha256": UNITY_PINS, "excluded_response_hop_tests": sorted(excluded),
        "hardware": False, "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
