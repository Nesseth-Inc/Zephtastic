#!/usr/bin/env python3
"""Render canned-route views on all three production display geometries."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path

TEST = Path(__file__).resolve().with_name("m141_canned_route_render_tests.cpp")
APP = TEST.parents[1]
REPO = TEST.parents[2]
RUNNER = Path(__file__).resolve()
INPUTS = [TEST, RUNNER, APP / "src/ui/Model.h", APP / "src/ui/Render.h",
          APP / "src/ui/Display.h", APP / "src/ui/Display.cpp"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or output.is_relative_to(REPO):
        parser.error("output must be fresh and outside the source worktree")
    before = {str(path): digest(path) for path in INPUTS}
    output.mkdir(parents=True)
    includes = [APP / "src", REPO / "zephcore/adapters/bench/vendor",
                REPO / "zephcore/bench/upstream-differential/source",
                REPO / "zephcore/bench/upstream-differential/source/nanopb"]
    variants = [
        ("oled-128x64", ["-DCONFIG_MT_UI_WIDTH=128", "-DCONFIG_MT_UI_HEIGHT=64",
                         "-DCONFIG_MT_UI_MONO=1", "-DCONFIG_MT_UI_STRIP_ROWS=64"]),
        ("tft-160x80", ["-DCONFIG_MT_UI_WIDTH=160", "-DCONFIG_MT_UI_HEIGHT=80",
                         "-DCONFIG_MT_UI_MONO=0", "-DCONFIG_MT_UI_STRIP_ROWS=80"]),
        ("wide-240x135", ["-DCONFIG_MT_UI_WIDTH=240", "-DCONFIG_MT_UI_HEIGHT=135",
                           "-DCONFIG_MT_UI_MONO=0", "-DCONFIG_MT_UI_STRIP_ROWS=135"]),
    ]
    cxx = shlex.split(os.environ.get("CXX", "clang++"))
    results = []
    for name, geometry in variants:
        binary = output / name
        cmd = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *geometry,
               *["-I" + str(path) for path in includes], str(TEST), "-o", str(binary)]
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1"}
        build = subprocess.run(cmd, cwd=REPO, env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        log = "$ " + shlex.join(cmd) + "\n" + build.stdout
        if build.returncode:
            (output / (name + ".log")).write_text(log)
            raise RuntimeError("COMPILE_FAILED:" + name)
        run = subprocess.run([str(binary)], cwd=REPO, env=env, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        log += "$ " + str(binary) + "\n" + run.stdout
        (output / (name + ".log")).write_text(log)
        if run.returncode:
            raise RuntimeError("FIXTURE_FAILED:" + name)
        results.append({"variant": name, "status": "PASS",
                        "binary_sha256": digest(binary),
                        "log_sha256": digest(output / (name + ".log"))})
    after = {str(path): digest(path) for path in INPUTS}
    if before != after:
        raise RuntimeError("SOURCE_CHANGED_DURING_TEST")
    result = {"schema": "zt-m141-canned-route-render-native/v1", "status": "PASS",
              "input_sha256": before, "source_stable_during_test": True,
              "ASAN_UBSAN": True, "hardware": False, "radio": False,
              "network": False, "target_build": False, "variants": results}
    (output / "RESULT.json").write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "variants": len(results), "source_stable": True}))


if __name__ == "__main__":
    main()
