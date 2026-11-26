#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import os
import shlex
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STUBS = ROOT / "mt_only/tests/m138_gpio_backend_stubs"
SOURCE = ROOT / "mt_only/src/modules/ZephyrGpioModule.cpp"
TEST = ROOT / "mt_only/tests/m138_gpio_backend_tests.cpp"


def run(argv, env):
    print("$", shlex.join(map(str, argv)), flush=True)
    subprocess.run(argv, check=True, env=env)


def main():
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    variants = [
        ("valid", []),
        ("empty-node", ["FAKE_GPIO_NO_NODE=1"]),
        ("startup-unready", ["FAKE_GPIO_START_UNREADY=1"]),
        ("bad-wire-id", ["FAKE_GPIO_BAD_WIRE_ID=1"]),
        ("duplicate-wire-id", ["FAKE_GPIO_DUPLICATE_WIRE_ID=1"]),
        ("duplicate-physical-pin", ["FAKE_GPIO_DUPLICATE_PHYSICAL_PIN=1"]),
        ("pullup-without-read", ["FAKE_GPIO_BAD_PULLUP=1"]),
    ]
    with tempfile.TemporaryDirectory(prefix="m138-gpio-backend-") as temp:
        out = Path(temp)
        for name, defines in variants:
            exe = out / name
            argv = [
                *cxx,
                "-std=c++17",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-g",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(STUBS),
                "-I",
                str(ROOT / "mt_only/src/modules"),
                *[f"-D{define}" for define in defines],
                str(TEST),
                str(SOURCE),
                "-o",
                str(exe),
            ]
            run(argv, env)
            run([str(exe)], env)
            symbols = subprocess.check_output(["nm", "-u", str(exe)], text=True, env=env)
            if any(name in symbols for name in ("__cxa_guard_acquire", "__cxa_guard_release", "__cxa_guard_abort")):
                raise SystemExit("backend requires unavailable target C++ local-static guard runtime")

        # The backend deliberately supports one unambiguous dedicated node.
        # A board overlay with two enabled instances must fail compilation.
        argv = [
            *cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(STUBS),
            "-I",
            str(ROOT / "mt_only/src/modules"),
            "-DFAKE_GPIO_MULTIPLE_NODES=1",
            "-c",
            str(SOURCE),
            "-o",
            str(out / "multiple-nodes.o"),
        ]
        print("$", shlex.join(argv), flush=True)
        result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        if result.returncode == 0 or "GPIO module exports require at most one enabled dedicated node" not in result.stdout:
            raise SystemExit("multiple enabled export nodes did not fail at the intended compile assertion")
        print("PASS M138 multiple enabled export nodes rejected at compile time")
    print("PASS M138 fake-Zephyr GPIO backend matrix")


if __name__ == "__main__":
    main()
