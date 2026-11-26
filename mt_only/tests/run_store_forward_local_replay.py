#!/usr/bin/env python3
"""Native sanitizer matrix for StoreForward local PhoneAPI history replay."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
TEST = Path(__file__).with_name("store_forward_local_replay_tests.cpp")


def run(argv, env):
    print("$", " ".join(map(str, argv)), flush=True)
    result = subprocess.run(list(map(str, argv)), env=env, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)


def main():
    if not (PROTO / "mesh.pb.h").is_file() or not TEST.is_file():
        raise SystemExit("generated protobuf source or test is missing")
    cc = shlex.split(os.environ.get("CXX", "c++"))
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    with tempfile.TemporaryDirectory(prefix="m136-local-history-") as directory:
        build = Path(directory)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            (generated / header.name).write_bytes(header.read_bytes())
        common = ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                  "-g", "-O1", "-I" + str(build),
                  "-I" + str(PROTO / "nanopb"), "-I" + str(ROOT / "mt_only/src")]
        for label, flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            exe = build / ("local-history-" + label)
            run([*cc, *flags, *common, TEST, "-o", exe], env)
            run([exe], env)


if __name__ == "__main__":
    main()
