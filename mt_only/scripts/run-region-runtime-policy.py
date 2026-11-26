#!/usr/bin/env python3
"""Run local RegionRuntimePolicy and ModuleController policy cases."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


parser = argparse.ArgumentParser()
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--short-enums", action="store_true")
args = parser.parse_args()

app = Path(__file__).resolve().parents[1]
repo = app.parent
vendor = repo / "zephcore/adapters/bench/vendor"
mono = repo / "zephcore/lib/monocypher"
proto = repo / "zephcore/bench/upstream-differential/source"
out = args.output.expanduser().absolute()
if out.exists() or out.is_symlink():
    raise SystemExit("REFUSE_EXISTING_OUTPUT")
out.parent.mkdir(parents=True, exist_ok=True)
out.mkdir(mode=0o700, exist_ok=False)
(out / "meshtastic").mkdir()
for header in proto.glob("*.pb.h"):
    shutil.copyfile(header, out / "meshtastic" / header.name)

cc = shlex.split(os.environ.get("CC", "clang"))
cxx = shlex.split(os.environ.get("CXX", "clang++"))
flags = ["-fsanitize=address,undefined", "-g", "-I" + str(app / "src"),
         "-I" + str(vendor), "-I" + str(mono), "-I" + str(out),
         "-I" + str(proto / "nanopb")]
host_link: list[str] = []
if sys.platform.startswith("linux"):
    flags.append("-DMT_HOST_OPENSSL=1")
    host_link.append("-lcrypto")
if args.short_enums:
    flags.append("-fshort-enums")

commands: list[list[str]] = []
objects: list[str] = []
for src in [mono / "monocypher.c", mono / "monocypher-ed25519.c",
            *(proto / "nanopb" / name for name in
              ("pb_common.c", "pb_encode.c", "pb_decode.c"))]:
    obj = out / (src.stem + ".o")
    cmd = [*cc, "-std=c11", *flags, "-c", str(src), "-o", str(obj)]
    subprocess.run(cmd, check=True)
    commands.append(cmd)
    objects.append(str(obj))

test = app / "tests/region_runtime_policy_cases.cpp"
cmd = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
       str(test), str(app / "src/Protobuf.cpp"), str(vendor / "MtCodec.cpp"),
       str(vendor / "MtSignature.cpp"), *objects, *host_link,
       "-o", str(out / "region-runtime-policy-cases")]
subprocess.run(cmd, check=True)
commands.append(cmd)

env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
run = subprocess.run([str(out / "region-runtime-policy-cases")],
                     check=False, capture_output=True, text=True, env=env)
(out / "runtime-policy.log").write_text(run.stdout + run.stderr)
source_files = [Path(__file__).resolve(), test, app / "src/RegionRuntimePolicy.h",
                app / "src/RegionPolicy.h", app / "src/TxAdmission.h",
                app / "src/ModuleController.h", app / "src/ModuleIntervals.h",
                app / "src/OnlineObservations.h", app / "src/Service.h",
                app / "src/Config.h"]
summary = {
    "status": "PASS" if run.returncode == 0 else "FAIL",
    "returncode": run.returncode,
    "ASAN_UBSAN": True,
    "short_enums": args.short_enums,
    "hardware": False,
    "RF": False,
    "groups": sum(line.startswith("PASS ") for line in run.stdout.splitlines()),
    "commands": commands,
    "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in source_files},
    "scope": "regional duty and ModuleController interval policy; fake radio, no physical or RF operation",
}
(out / "RESULT.json").write_text(json.dumps(summary, sort_keys=True, indent=2) + "\n")
print(run.stdout, end="")
if run.returncode:
    print(run.stderr, end="")
    raise SystemExit(run.returncode)
