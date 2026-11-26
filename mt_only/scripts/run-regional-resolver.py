#!/usr/bin/env python3
"""Run the local regional resolver differential under ASan/UBSan."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess


parser = argparse.ArgumentParser()
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--short-enums", action="store_true")
args = parser.parse_args()

app = Path(__file__).resolve().parents[1]
repo = app.parent
out = args.output.expanduser().absolute()
if out.exists() or out.is_symlink():
    raise SystemExit("REFUSE_EXISTING_OUTPUT")
out.parent.mkdir(parents=True, exist_ok=True)
out.mkdir(mode=0o700, exist_ok=False)

stock = repo / "zephcore/bench/upstream-differential/source/RadioInterface.cpp"
stock_sha = "0de8434b14c3c1deb383b4510b96a9d0d750a797e9140f9cfb08147e4a749506"
if hashlib.sha256(stock.read_bytes()).hexdigest() != stock_sha:
    raise SystemExit("PINNED_STOCK_RADIOINTERFACE_MISMATCH")

inputs = [
    Path(__file__).resolve(),
    app / "tests/regional_resolver_cases.cpp",
    app / "src/RadioSettings.h",
    app / "src/USChannel.h",
    app / "src/RegionPolicy.h",
    app / "src/LoRaPresets.h",
    app / "src/ManualLoRa.h",
    stock,
]
source_pins = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
               for path in inputs}

cmd = [
    *shlex.split(os.environ.get("CXX", "clang++")),
    "-std=c++17", "-Wall", "-Wextra", "-Werror",
    "-fsanitize=address,undefined", "-g",
]
if args.short_enums:
    cmd.append("-fshort-enums")
cmd.extend(("-I", str(app / "src"), str(app / "tests/regional_resolver_cases.cpp"),
            "-o", str(out / "regional-resolver-cases")))
subprocess.run(cmd, check=True)
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
result = subprocess.run([str(out / "regional-resolver-cases")],
                        check=False, capture_output=True, text=True, env=env)
(out / "regional-resolver.log").write_text(result.stdout + result.stderr)
(out / "RESULT.json").write_text(json.dumps({
    "status": "PASS" if result.returncode == 0 else "FAIL",
    "returncode": result.returncode,
    "ASAN_UBSAN": True,
    "short_enums": args.short_enums,
    "hardware": False,
    "RF": False,
    "commands": [cmd],
    "groups": sum(line.startswith("PASS ") for line in result.stdout.splitlines()),
    "source_sha256": source_pins,
    "scope": "regional frequency/preset/slot resolver only; no settings persistence, driver IO, or RF",
}, sort_keys=True, indent=2) + "\n")
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
