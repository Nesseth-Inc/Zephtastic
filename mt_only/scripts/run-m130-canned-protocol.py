#!/usr/bin/env python3
"""Run local M130 canned Admin protocol cases with fake storage/radio only."""
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

parser = argparse.ArgumentParser(description=__doc__)
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
    command = [*cc, "-std=c11", *flags, "-c", str(src), "-o", str(obj)]
    subprocess.run(command, check=True)
    commands.append(command)
    objects.append(str(obj))

test = app / "tests/m130_canned_protocol_cases.cpp"
command = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
           str(test), str(app / "src/Protobuf.cpp"), str(vendor / "MtCodec.cpp"),
           str(vendor / "MtSignature.cpp"), *objects, *host_link,
           "-o", str(out / "m130-canned-protocol-cases")]
subprocess.run(command, check=True)
commands.append(command)
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
result = subprocess.run([str(out / "m130-canned-protocol-cases")],
                        check=False, capture_output=True, text=True, env=env)
(out / "runtime.log").write_text(result.stdout + result.stderr)
sources = [Path(__file__).resolve(), test, app / "src/Companion.h",
           app / "src/Service.h", app / "src/Endpoint.h",
           proto / "admin.pb.h", proto / "nanopb/pb_encode.c",
           proto / "nanopb/pb_decode.c"]
summary = {
    "status": "PASS" if result.returncode == 0 else "FAIL",
    "returncode": result.returncode,
    "ASAN_UBSAN": True,
    "short_enums": args.short_enums,
    "hardware": False,
    "RF": False,
    "physical_radio": False,
    "groups": sum(line.startswith("PASS ") for line in result.stdout.splitlines()),
    "commands": commands,
    "source_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in sources},
    "scope": "stock Admin canned-list protocol with fake store/radio plus ordinary text queue-full path",
}
(out / "RESULT.json").write_text(json.dumps(summary, sort_keys=True, indent=2) + "\n")
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
