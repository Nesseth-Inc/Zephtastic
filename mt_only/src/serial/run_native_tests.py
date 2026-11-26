#!/usr/bin/env python3
"""Build and run bounded serial behavior tests with host C++17."""
import pathlib, subprocess, tempfile

here = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="mt-serial-test-") as tmp:
    exe = pathlib.Path(tmp) / "serial-tests"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                   "-I", str(here), str(here / "SerialEngine.cpp"), str(here / "serial_engine_tests.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS serial engine: timeout, cap, ports/channels/ACK, raw/text receive, echo dedup")
