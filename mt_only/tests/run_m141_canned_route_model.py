#!/usr/bin/env python3
"""Focused ASan/UBSan test for bounded canned-message route selection."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path

TEST = Path(__file__).resolve().parent / "m141_canned_route_model_tests.cpp"
MODEL = TEST.parents[1] / "src/ui/Model.h"
RUNNER = Path(__file__).resolve()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or output.is_relative_to(TEST.parents[2]):
        parser.error("output must be fresh and outside the source worktree")
    before = {str(path): digest(path) for path in (TEST, MODEL, RUNNER)}
    output.mkdir(parents=True)
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    cxx = shlex.split(env.get("CXX", "clang++"))
    results = []
    for name, flags in (("default-enum", []), ("short-enum", ["-fshort-enums"])):
        binary = output / ("route-model-" + name)
        cmd = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *flags,
               str(TEST), "-o", str(binary)]
        proc = subprocess.run(cmd, cwd=TEST.parents[2], env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=180)
        log = "$ " + shlex.join(cmd) + "\n" + proc.stdout
        (output / (name + ".log")).write_text(log)
        if proc.returncode:
            raise RuntimeError("FIXTURE_FAILED:" + name + ":" + str(proc.returncode))
        run = subprocess.run([str(binary)], cwd=TEST.parents[2], env=env, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=30)
        with (output / (name + ".log")).open("a") as stream:
            stream.write("$ " + str(binary) + "\n" + run.stdout)
        if run.returncode:
            raise RuntimeError("TEST_FAILED:" + name + ":" + str(run.returncode))
        results.append({"variant": name, "status": "PASS", "output": run.stdout.strip(),
                        "binary_sha256": digest(binary),
                        "log_sha256": digest(output / (name + ".log"))})
    after = {str(path): digest(path) for path in (TEST, MODEL, RUNNER)}
    if before != after:
        raise RuntimeError("INPUT_CHANGED_DURING_TEST")
    result = {"schema": "zt-m141-canned-route-model-native/v1", "status": "PASS",
              "input_sha256": before, "source_stable_during_test": True,
              "ASAN_UBSAN": True, "hardware": False, "radio": False,
              "network": False, "target_build": False, "executions": results}
    (output / "RESULT.json").write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "variants": len(results), "source_stable": True}))


if __name__ == "__main__":
    main()
