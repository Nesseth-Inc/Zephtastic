#!/usr/bin/env python3
"""Build and run native lifecycle checks for the shared Companion scratch lease."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument("--source", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
root = a.source.resolve()
out = a.output.resolve()
if out.exists() or out.is_relative_to(root):
    p.error("output must be fresh and outside source")
app = root / "mt_only"
proto = root / "zephcore/bench/upstream-differential/source"
nanopb = proto / "nanopb"
test = app / "tests/m147_companion_workspace_lease_tests.cpp"
out.mkdir(parents=True)
generated = out / "generated/meshtastic"
generated.mkdir(parents=True)
for header in proto.glob("*.pb.h"):
    shutil.copyfile(header, generated / header.name)

cxx = shlex.split(os.environ.get("CXX", "clang++"))
results = []
for name, enum_flags in (("default", []), ("short-enums", ["-fshort-enums"])):
    binary = out / name
    command = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *enum_flags,
               "-I" + str(app / "src"), "-I" + str(app / "src/admin"),
               "-I" + str(generated.parent),
               "-I" + str(nanopb), str(test), "-o", str(binary)]
    build = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=180)
    (out / (name + ".build.log")).write_text(build.stdout)
    if build.returncode:
        raise SystemExit("BUILD_FAILED:" + name)
    run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=30)
    (out / (name + ".run.log")).write_text(run.stdout)
    results.append({"mode": name, "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                    "returncode": run.returncode, "stdout": run.stdout.strip()})
    if run.returncode:
        raise SystemExit("RUN_FAILED:" + name)
result = {"status": "PASS", "target": False, "hardware": False,
          "source_head": subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
          "workspace_header_sha256": hashlib.sha256((app / "src/admin/CompanionWorkspace.h").read_bytes()).hexdigest(),
          "test_sha256": hashlib.sha256(test.read_bytes()).hexdigest(), "results": results}
(out / "RESULT.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
