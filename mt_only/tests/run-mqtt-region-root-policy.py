#!/usr/bin/env python3
"""Build/run the standalone regional MQTT root policy fixture."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "mt_only"


def main():
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-region-root-") as tmp:
        exe = Path(tmp) / "mqtt-region-root-policy"
        command = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-g", "-O1", "-I" + str(APP / "src"),
                   APP / "tests/mqtt_region_root_policy_tests.cpp", "-o", exe]
        print("$", " ".join(map(str, command)), flush=True)
        subprocess.run(list(map(str, command)), check=True)
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
