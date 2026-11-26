#!/usr/bin/env python3
"""Sanitized M115 bounded decoder and pinned-stock compatibility fixtures."""
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
APP = ROOT / "mt_only"
REPO = ROOT
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"
BASELINE = "2d11cfb22c97017e4411cfd6ebe4c93f1d6e7cd6"
STOCK_COMMIT = "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
INPUT_HASHES = {
    "mt_only/src/MqttServiceEnvelopeCodec.h": "8dfe7c574ccae176542cb73f734903aafea8acf1fa58497c5e1dd28e6dafd563",
    "zephcore/bench/upstream-differential/source/source-lock.json": "c3c4898db62e9e62ed67d242b87014d79b72f2c67c11d6193af6300f2190b03d",
}


def run(command, env):
    print("$", shlex.join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), check=False, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    print(result.stdout, end="")
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command)


def main():
    head = subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "HEAD"], text=True).strip()
    if subprocess.run(["git", "-C", str(REPO), "merge-base", "--is-ancestor", BASELINE, head], capture_output=True).returncode != 0:
        raise RuntimeError(f"expected M115 baseline ancestry {BASELINE}, found {head}")
    lock = json.loads((PROTO / "source-lock.json").read_text())
    if lock.get("commit") != STOCK_COMMIT:
        raise RuntimeError("generated schema source-lock revision differs from stock MQTT.cpp revision")
    for relative, expected in INPUT_HASHES.items():
        actual = hashlib.sha256((ROOT / relative).read_bytes()).hexdigest()
        if actual != expected:
            raise RuntimeError(f"pinned decoder input changed: {relative}")
    # Protobuf.cpp is intentionally not a historical hash pin: M116 adds the
    # production MapReport binding there. Keep its exact build input visible
    # in the run evidence while the immutable schema/runtime inputs stay pinned.
    production_bindings = ROOT / "mt_only/src/Protobuf.cpp"
    binding_hash = hashlib.sha256(production_bindings.read_bytes()).hexdigest()
    print(f"EXECUTION_INPUT mt_only/src/Protobuf.cpp sha256={binding_hash}", flush=True)
    for item in lock["files"]:
        local = item.get("local_file", "")
        if local.endswith(".pb.h") or (local.startswith("nanopb/") and
                                          local.endswith((".c", ".h"))):
            path = PROTO / local
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
                raise RuntimeError(f"pinned generated/protobuf runtime input changed: {local}")

    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"

    with tempfile.TemporaryDirectory(prefix="mt-mqtt-downlink-decoder-") as temporary:
        build = Path(temporary)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            (generated / header.name).write_bytes(header.read_bytes())

        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g1", "-O1",
                  "-I" + str(build), "-I" + str(NANOPB), "-I" + str(APP / "src")]
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            # Production decoder: compile without PB_ENABLE_MALLOC.
            objects = []
            for source in (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c"):
                obj = build / (label + "-" + source.stem + ".o")
                run([*cc, "-std=c11", *common, *enum_flags, "-Wall", "-Wextra", "-c", source, "-o", obj], env)
                objects.append(obj)
            decoder_exe = build / ("mqtt-downlink-decoder-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags, *common,
                 APP / "tests/mqtt_downlink_decoder/decoder_tests.cpp",
                 APP / "src/Protobuf.cpp", *objects, "-o", decoder_exe], env)
            run([decoder_exe], env)

            # Stock ServiceEnvelope fields are pointer-backed; emulate the
            # pinned DecodedServiceEnvelope pb_decode configuration here only.
            stock_objects = []
            stock_flags = [*common, "-DPB_ENABLE_MALLOC=1", *enum_flags]
            for source in (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c"):
                obj = build / (label + "-stock-" + source.stem + ".o")
                run([*cc, "-std=c11", *stock_flags, "-Wall", "-Wextra", "-c", source, "-o", obj], env)
                stock_objects.append(obj)
            stock_exe = build / ("mqtt-downlink-stock-compatibility-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *stock_flags,
                 APP / "tests/mqtt_downlink_decoder/stock_compatibility_tests.cpp",
                 APP / "src/Protobuf.cpp", *stock_objects, "-o", stock_exe], env)
            run([stock_exe], env)


if __name__ == "__main__":
    main()
