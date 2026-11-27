#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Collect the qualification receipts for a single Actions job."""
import argparse
import json
import os
from pathlib import Path

from remaining_coverage import verify_module_evidence

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("group")
parser.add_argument("work", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
boards = {"nordic": ["t096-680", "t096-728", "t114-v2", "rak4631-mini"],
          "esp32": ["xiao-ordinary", "v4.2-r2", "v4.3-r2", "v4.3-r8"],
          "router": ["t096-router-680"]}
revision = os.environ["GITHUB_SHA"]
report = {"commit": revision, "group": args.group, "hardware": False}

def read(name):
    return json.loads((args.work / name).read_text())

if args.group == "native":
    upstream = read("upstream/results.json")
    owners = upstream["results"]["next_five_owners"]
    for name in ("FloodingRouter", "MeshModule"):
        comparisons = owners[name]["comparisons"]
        if type(comparisons) is not int or comparisons <= 0:
            raise ValueError(f"Missing 2.8.2 comparison for {name}")
    report["upstream"] = upstream
    report["node_info_and_position"] = read("node-position.json")
    report["device_config"] = read("device-config.json")
    report["module_parity"] = {}
    for suite, families in (
            ("stock_traffic_position", ("traffic_management", "position")),
            ("stock_hop_beacon", ("hop_scaling", "mesh_beacon")),
            ("stock_telemetry_parity", ("telemetry",))):
        evidence = read(f"{suite}.json")
        if evidence["stock_revision"] != upstream["stock_revision"]:
            raise ValueError(f"{suite} used a different upstream revision")
        for family in families:
            count = evidence["comparisons"][family]
            if type(count) is not int or count <= 0:
                raise ValueError(f"{suite} did not compare {family}")
        report["module_parity"][suite] = evidence
    persistence = read("settings-journal/results.json")
    canned = read("canned-journal.json")
    size = persistence["simulated_record_size"]
    if (persistence["PASS"] is not True or persistence["ASAN_UBSAN"] is not True or
            persistence["hardware"] is not False or size <= 0 or
            persistence["partial_write_cut_points_per_path"] != size or
            persistence["corruption_single_bits"] != 8 * size or
            canned["groups"] != {"default-enum": 16, "short-enum": 16} or
            canned["asan_ubsan"] is not True or canned["hardware"] is not False):
        raise ValueError("Incomplete journal fault and recovery evidence")
    report["settings_journals"] = {
        "partial_write_cut_points_per_path": persistence["partial_write_cut_points_per_path"],
        "corruption_single_bits": persistence["corruption_single_bits"],
        "canned_message_groups": canned["groups"],
        "asan_ubsan": True,
    }
    history = read("transmit-history/result.json")
    expected = {(abi, case) for abi in ("normal", "short")
                for case in ("history", "producers", "beacon", "factory")}
    if {(row["enum"], row["case"]) for row in history["tests"]} != expected:
        raise ValueError("Incomplete broadcast history regression coverage")
    report["transmit_history"] = history
    router = read("router-policy/results.json")
    if router["status"] != "PASS" or router["groups"] != 4:
        raise ValueError("Incomplete T096 router policy evidence")
    report["router_policy"] = router
    report["roles"] = {}
    for abi in ("native", "short"):
        roles = read(f"roles-{abi}/results.json")
        timing = read(f"roles-{abi}/stock-timing/results.json")
        if (roles["status"] != "PASS" or roles["groups"] < 1 or
                roles["short_enums"] != (abi == "short") or timing["status"] != "PASS" or
                timing["short_enums"] != (abi == "short") or
                timing["stock_commit"] != upstream["stock_revision"] or timing["comparisons"] < 1):
            raise ValueError(f"Incomplete role evidence for {abi}")
        report["roles"][abi] = {"role_channel": roles, "timing": timing}
elif args.group.startswith("modules-"):
    evidence = read("modules.json")
    verify_module_evidence(args.group.removeprefix("modules-"), evidence)
    report["module_evidence"] = evidence
    if args.group == "modules-core":
        hook = evidence["modules"]["MeshBeaconTxHook"]["runtime"]
        if {row["enum"] for row in hook["runs"]} != {"normal", "short"}:
            raise ValueError("Missing beacon recovery enum mode")
        for row in hook["runs"]:
            faults = row["usb_recovery"]
            if set(faults) != {"lost", "home"}:
                raise ValueError("Missing USB session during a beacon fault")
            for session in faults.values():
                if (session["stock_handshakes"] < 2 or session["existing_session_read_cycles"] < 2 or
                        session["fresh_session_read_cycles"] < 2 or session["metadata_status_replies"] < 2 or
                        session["heartbeat_replies"] < 2 or session["denied_writes_and_sends"] < 2 or
                        not session["epoch_split_rejected"] or not session["ordinary_read_after_release"]):
                    raise ValueError("Incomplete USB recovery session")
        activity = read("radio-activity/SUMMARY.json")
        if activity["status"] != "PASS" or not activity["main_drain_observe_admin_order"]:
            raise ValueError("Incomplete native radio and USB service-order checks")
        report["native_radio_service_order"] = True
        restart = read("hop-restart.json")
        nvs = restart["nvs"]
        if (restart["stock_revision"] != evidence["stock_revision"] or
                restart["status"] != "PASS" or restart["outstanding_admin_query"] != "PASS" or nvs["status"] != "PASS" or
                {run["enum"] for run in restart["runs"]} != {"normal", "short"} or
                {row["sectors"] for row in nvs["runs"]} != {4, 8} or
                any(row["interruption_cases"] <= 0 or row["checkpoints_with_collection"] <= 0
                    for row in nvs["runs"])):
            raise ValueError("Incomplete HopScaling restart or storage evidence")
        report["hop_restart"] = restart
else:
    profiles = {}
    for profile in boards[args.group]:
        measured = read(f"{profile}/memory.json")
        if measured["profile"] != profile or measured["commit"] != revision:
            raise ValueError(f"Build receipt does not identify {profile} at {revision}")
        profiles[profile] = measured
    report["profiles"] = profiles

args.output.parent.mkdir(exist_ok=True, parents=True)
args.output.write_text(json.dumps(report, indent=2) + "\n")
