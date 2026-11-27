#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the public application and service comparisons against Meshtastic 2.8.2."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


APP = Path(__file__).resolve().parents[2]
RELEASE = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
BASELINE = "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"


def run(script, *args):
    result = subprocess.run([sys.executable, APP / script, *args], text=True,
                            stdout=subprocess.PIPE)
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    return result.stdout


def require(output, pattern, count):
    matches = re.findall(pattern, output, flags=re.MULTILINE)
    if len(matches) != count:
        raise RuntimeError(f"Expected {count} occurrences of {pattern!r}; observed {len(matches)}")
    return matches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    for revision in (RELEASE, BASELINE):
        subprocess.run(["git", "-C", args.stock, "cat-file", "-e", revision + "^{commit}"], check=True)
    stock = ("--stock", str(args.stock.resolve()))
    release = (*stock, "--release", "2.8.2")
    modules = {}

    def passed(module, evidence, **details):
        modules[module] = {"kind": "differential", "evidence": evidence, **details}

    text = run("tests/stock_text_verification/run.py", *stock)
    counts = require(text, r"^PASS (\d+) pinned text classification", 2)
    if counts[0] != counts[1] or int(counts[0]) == 0:
        raise RuntimeError("Text cases disagree between enum ABIs")
    passed("TextMessageModule", "Compared stock port classification, bell alerts and wake policy across roles, battery states, mute and storage outcomes.",
           comparisons_per_abi=int(counts[0]))
    require(text, r"^PASS donor SHA-256 transcript, solicited proof, on-screen code and both local approvals$", 4)
    passed("KeyVerificationModule", "Pinned and locally staged synthetic peer exchanges produced the donor proof and display code, then required local approvals to commit trust.")

    trace = run("tests/stock_trace_hops/run.py", *release)
    counts = require(trace, r"^PASS (\d+) traceroute mutations", 2)
    if counts[0] != counts[1] or int(counts[0]) == 0:
        raise RuntimeError("TraceRoute cases disagree between enum ABIs")
    passed("TraceRouteModule", "Compared forward and return route arrays, missing hops, terminal signal-only updates and quarter-dB SNR saturation.",
           comparisons_per_abi=int(counts[0]))

    gateway = run("tests/stock_gateway_modules/run.py", *stock)
    require(gateway, r"^PASS default-enums$", 1)
    require(gateway, r"^PASS short-enums$", 1)
    service = run("tests/stock_service_modules/run.py", *release)
    require(service, r"^PASS pinned MQTT packet selection, GPIO commands/watch, StoreForward history/stats, AQ/health acquisition$", 2)
    passed("StoreForwardModule", "Compared history reply shape, bounded replay, busy state, router stats, heartbeat timing and directed client updates with the 2.8.2 handler.")
    passed("MQTT", "Compared uplink selection for text, range and detection packets, subscriptions, proxy data and map report precision with stock routines.")

    waypoint = run("tests/stock_waypoint/run.py", *release)
    require(waypoint, r"^PASS stock Waypoint ownership, recency, local-only preferences, expiry, circle/box geofence crossings", 2)
    require(waypoint, r"^PASS loaded real Companion, USB/BLE single history", 2)
    passed("WaypointModule", "Compared stock waypoint storage, expiry, ownership and eviction with ZT history, then exercised companion ingress.")
    passed("GeofenceModule", "Compared stock circle and box classifications with local enter/exit events, including suppressed out-of-scope positions.")

    with tempfile.TemporaryDirectory(prefix="zt-wire-") as tmp:
        background = run("scripts/test-stock-background-modules.py", *release,
                         "--output", Path(tmp) / "background")
        require(background, r"PASS ATAK opaque application payloads including embedded NULs", 2)
        require(background, r"PASS status schedule, TX bytes and RX cache value", 2)
        require(background, r"PASS range heartbeat bytes, zero hops and RX log eligibility", 2)
        require(background, r"PASS range scheduler waits for a polite channel and preserves sequence", 2)
        require(background, r"PASS neighbor direct, legacy, fresh unique, ambiguous, stale and dummy RX observations", 2)
        for name, extra in (("default", []), ("short", ["--short-enums"])):
            controller = run("scripts/test-companion.py", "--module-config", "--output",
                             Path(tmp) / ("controller-" + name), *extra)
            require(controller, r"range sender skips unknown or 25-percent occupied channel and resumes at sequence one", 1)

    passed("NeighborInfoModule", "Compared table updates, expiry, scheduling and receiver attribution including direct, ambiguous, stale and dummy observations.")
    passed("RangeTestModule", "Compared scheduler admission, sequence bytes, hop limit and receive logging policy; the controller skipped unknown and occupied channels and resumed at sequence one.")
    passed("StatusMessageModule", "Compared the status schedule, encoded outbound bytes and decoded peer cache values for empty, typical and maximum-length status strings.")
    passed("AtakPluginModule", "Ran the donor pass-through handler and compared opaque port-78 application payload bytes after production encoding and decoding.")

    receipt = {"stock_revision": RELEASE, "modules": modules,
               "enum_abis": ["default", "short"],
               "limits": ["Host simulations use synthetic radio, clock, storage and keys.",
                          "Local key confirmation, broker opt-out, bounded replay, canonical status inputs and the eight-hour range cutoff retain ZT policy."]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # The evidence path becomes readable only after every required group passes.
    with tempfile.NamedTemporaryFile(mode="w", dir=args.output.parent, delete=False) as pending:
        json.dump(receipt, pending, indent=2, sort_keys=True)
        pending.write("\n")
    os.replace(pending.name, args.output)


if __name__ == "__main__":
    main()
