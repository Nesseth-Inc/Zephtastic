#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Collect the qualification receipts for a single Actions job."""
import argparse
import json
import os
from pathlib import Path

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
