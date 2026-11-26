#!/usr/bin/env python3
"""Run the focused MQTT certificate-clock state tests with sanitizers."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "mt_only"


def main():
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-tls-clock-") as tmp:
        exe_dir = Path(tmp)
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            exe = exe_dir / ("mqtt-tls-clock-" + label)
            command = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                       *enum_flags, "-I" + str(APP / "src"),
                       APP / "tests/mqtt_tls_clock_tests.cpp", "-o", exe]
            print("$", " ".join(map(str, command)), flush=True)
            subprocess.run(list(map(str, command)), check=True)
            print(label + ":", flush=True)
            subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
