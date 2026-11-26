#!/usr/bin/env python3
"""Run the bounded M129 region table fixture under native sanitizers."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "mt_only"
SOURCE = APP / "tests/region_policy_cases.cpp"


def main():
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="mt-region-policy-") as tmp:
        output = Path(tmp)
        for label, extra in (("default-enum", []), ("short-enum", ["-fshort-enums"])):
            exe = output / ("region-policy-" + label)
            command = [
                *cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-g",
                "-O1",
                *extra,
                "-I" + str(APP / "src"),
                str(SOURCE),
                "-o",
                str(exe),
            ]
            print("$", " ".join(command), flush=True)
            subprocess.run(command, check=True)
            subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
