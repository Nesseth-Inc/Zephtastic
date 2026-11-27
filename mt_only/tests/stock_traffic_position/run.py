#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run pinned donor predicates alongside real ZT ingress, routing and radio output."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "upstream_282"))
from phone_config import build_fixture

PIN = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
HASHES = {
    "src/modules/TrafficManagementModule.cpp": "9bea81e8fa22e769ad24c5588e1266aadd67914811e5555ce0abfe4671d341c6",
    "src/modules/TrafficManagementModule.h": "6e39ae7ad543ce64093ae5aade47209ddc4d9795cad59f0de97fcfb3d7f93b22",
    "src/mesh/PositionPrecision.cpp": "d15ff5646e8d3206fe7f3b5c99dbec43b657930e5aca1d7a4e1ef13a79e62d76",
    "src/DisplayFormatters.cpp": "2cc4e601afba55213e765df074bedee0348ffe2b3c64034a87d5107158ab4349",
    "src/mesh/Channels.cpp": "509d7b9d7c82ee64f4ec3b49629083c63705c0cf5ec52b0515052c1356ee60ff",
    "src/mesh/Default.h": "0a468de454cb7da41536b90715cde20d506b2430d3577310a32347b67fed0116",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sources = {}
    for name, digest in HASHES.items():
        data = (args.stock / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Expected {PIN} source for {name}")
        sources[Path(name).name] = data.decode()
    extract = runpy.run_path(str(ROOT / "zephcore/bench/run-routing-differential.py"))["extract"]
    with tempfile.TemporaryDirectory() as temp:
        out = Path(temp)
        bodies = []
        for name, markers in [
            ("DisplayFormatters.cpp", ["const char *DisplayFormatters::getModemPresetDisplayName("]),
            ("Channels.cpp", ["const char *Channels::getName(", "bool Channels::isWellKnownChannel("]),
            ("PositionPrecision.cpp", ["uint32_t getPositionPrecisionForChannel(const meshtastic_Channel &",
                                       "uint32_t getPositionPrecisionForChannel(uint8_t",
                                       "int32_t truncateCoordinate(int32_t coordinate, uint32_t",
                                       "void applyPositionPrecision(meshtastic_Position &"]),
            ("TrafficManagementModule.cpp", ["uint32_t secsToMs(", "uint8_t sanitizePositionPrecision(",
                    "uint8_t TrafficManagementModule::computePositionFingerprint(",
                    "bool TrafficManagementModule::shouldDropPosition(", "bool TrafficManagementModule::isRateLimited("]),
        ]:
            for marker in markers:
                bodies.append(extract(sources[name], marker)[0])
        (out / "donor.inc").write_text("\n".join(bodies))
        defaults = [line for line in sources["Default.h"].splitlines() if
                    re.match(r"#define (default_position_stationary_broadcast_secs|default_traffic_mgmt_(position_precision_bits|position_min_interval_secs|tracker_position_min_interval_secs|lost_and_found_position_min_interval_secs)) ", line)]
        ticks = [line for line in sources["TrafficManagementModule.h"].splitlines() if
                 re.search(r"static constexpr uint32_t k(Pos|Rate)TimeTickMs|static uint8_t current(Pos|Rate)Tick\(", line)]
        if len(defaults) != 5 or len(ticks) != 4:
            raise ValueError("Incomplete donor timebase")
        (out / "defaults.inc").write_text("\n".join(defaults) + "\n")
        (out / "ticks.inc").write_text("\n".join(ticks) + "\n")
        exe = build_fixture(out, HERE / "cases.cpp")
        counts = json.loads(subprocess.check_output([exe], text=True,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"}))
        if set(counts) != {"traffic_management", "position"} or not all(v > 0 for v in counts.values()):
            raise ValueError("Missing per-module wire comparisons")
    result = {"comparisons": counts, "stock_revision": PIN, "donor_sha256": HASHES}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(counts))


if __name__ == "__main__":
    main()
