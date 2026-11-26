#!/usr/bin/env python3
"""Run the pure MQTT reconnect policy against the production header."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

APP = Path(__file__).resolve().parents[1]


def main():
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="mt-mqtt-reconnect-") as td:
        build = Path(td)
        for label, enum_flags in (("default", []), ("short-enums", ["-fshort-enums"])):
            exe = build / ("reconnect-" + label)
            command = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                       *enum_flags, "-I" + str(APP / "src/network"),
                       APP / "tests/mqtt_reconnect_tests.cpp", "-o", exe]
            print("$", " ".join(map(str, command)), flush=True)
            subprocess.run(command, check=True)
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1"
            result = subprocess.run([str(exe)], check=False, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
            print(label + ":", flush=True)
            print(result.stdout, end="")
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
