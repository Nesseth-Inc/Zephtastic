#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Default/short-enum sanitizer fixture for the M139 TAK PLI payload projector."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
WT = APP.parent
SOURCE = WT / "zephcore/bench/upstream-differential/source"
NANOPB = SOURCE / "nanopb"
HEADER = APP / "src/modules/TakPliPayload.h"
TEST = APP / "tests/m139_tak_pli_tests.cpp"
RUNNER = Path(__file__).resolve()
PROTO_BINDING = APP / "src/Protobuf.cpp"
INPUTS = [
    HEADER, TEST, RUNNER, PROTO_BINDING,
    SOURCE / "source-lock.json", *sorted(SOURCE.glob("*.pb.h")),
    *[NANOPB / name for name in (
        "pb.h", "pb_common.h", "pb_encode.h", "pb_decode.h",
        "pb_common.c", "pb_encode.c", "pb_decode.c",
    )],
]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(argv, env, log):
    log.write("$ " + shlex.join(map(str, argv)) + "\n")
    log.flush()
    result = subprocess.run(list(map(str, argv)), cwd=WT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=180)
    log.write(result.stdout)
    log.flush()
    if result.returncode:
        raise RuntimeError("FIXTURE_COMMAND_FAILED:" + str(result.returncode))
    return result.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="fresh output directory outside the source worktree")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or output.is_relative_to(WT):
        parser.error("output must be fresh and outside the source worktree")

    before = {str(path): digest(path) for path in INPUTS}
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    cc = shlex.split(env.get("CC", "clang"))
    cxx = shlex.split(env.get("CXX", "clang++"))
    output.mkdir(parents=True)
    include = output / "include"
    include.mkdir()
    (include / "meshtastic").symlink_to(SOURCE, target_is_directory=True)
    common = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
              "-I", str(include), "-I", str(NANOPB), "-I", str(APP / "src")]
    executions = []
    for label, enum_flags in (("default-enum", []), ("short-enum", ["-fshort-enums"])):
        folder = output / label
        folder.mkdir()
        objects = []
        log_path = folder / "build.log"
        with log_path.open("x") as log:
            for index, name in enumerate(("pb_common.c", "pb_encode.c", "pb_decode.c")):
                source = NANOPB / name
                obj = folder / f"nanopb-{index}.o"
                run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                     *common, *enum_flags, "-c", str(source), "-o", str(obj)], env, log)
                objects.append(str(obj))
            binding_obj = folder / "protobuf.o"
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 *common, *enum_flags, "-c", str(PROTO_BINDING), "-o", str(binding_obj)], env, log)
            objects.append(str(binding_obj))
            test_obj = folder / "m139_tak_pli_tests.o"
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 *common, *enum_flags, "-c", str(TEST), "-o", str(test_obj)], env, log)
            objects.append(str(test_obj))
            binary = folder / "m139_tak_pli_tests"
            run([*cxx, *common, *enum_flags, *objects, "-o", str(binary)], env, log)
            stdout = run([str(binary)], env, log)
            executions.append({
                "variant": label,
                "status": "PASS",
                "test_output": stdout.strip(),
                "binary_sha256": digest(binary),
                "log_sha256": digest(log_path),
            })

    after = {str(path): digest(path) for path in INPUTS}
    if before != after:
        raise RuntimeError("SOURCE_CHANGED_DURING_FIXTURE")
    result = {
        "schema": "zt-m139-tak-pli-native/v2",
        "status": "PASS",
        "stock_pin": "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c",
        "source_stable": True,
        "input_sha256": before,
        "executions": executions,
        "ASAN_UBSAN": True,
        "hardware": False,
        "radio": False,
        "network": False,
        "target_build": False,
    }
    (output / "RESULT.json").write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "variants": len(executions), "source_stable": True}))

if __name__ == "__main__":
    main()
