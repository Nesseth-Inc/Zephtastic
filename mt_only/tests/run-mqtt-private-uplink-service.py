#!/usr/bin/env python3
"""ASan/UBSan native test against a caller-selected real M122 source tree."""
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import argparse

HERE = Path(__file__).resolve().parent

def run(argv, env):
    print("$", " ".join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), check=True, env=env)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2],
                    help="M122 checkout containing mt_only/src/Service.h")
    args = ap.parse_args()
    root = args.source.resolve(strict=True)
    sys.path.insert(0, str(root))
    import host_test_toolchain as host

    app = root / "mt_only"
    service = app / "src/Service.h"
    expected = ("using MqttPrivateTxSink=void(*)(void*,const uint8_t*,size_t,uint32_t);",
                "firstPrivateTxNotice(normalActive_)",
                "mqttPrivateTxSink_(mqttPrivateTxContext_,activeFrame_.bytes")
    text = service.read_text()
    for token in expected:
        if token not in text:
            raise SystemExit("source does not contain expected M122 Service callback path")
    proto = root / "zephcore/bench/upstream-differential/source"
    nanopb = proto / "nanopb"
    vendor = root / "zephcore/adapters/bench/vendor"
    mono = root / "zephcore/lib/monocypher"
    if not all(path.is_dir() for path in (proto, nanopb, vendor, mono)):
        raise SystemExit("source checkout is missing required native-test dependencies")

    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="m122-real-service-uplink-") as tmp:
        build = Path(tmp)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in proto.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                  "-DPB_ENABLE_MALLOC", "-DCONFIG_MT_MQTT_GATEWAY=1",
                  "-I" + str(build), "-I" + str(nanopb), "-I" + str(app / "src"),
                  "-I" + str(app / "tests"), "-I" + str(vendor), "-I" + str(mono),
                  *host.crypto_compile_flags()]
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        c_sources = (nanopb / "pb_common.c", nanopb / "pb_encode.c", nanopb / "pb_decode.c",
                     mono / "monocypher.c", mono / "monocypher-ed25519.c")
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            objects = []
            for source in c_sources:
                obj = build / (label + "-" + source.stem + ".o")
                run([*cc, "-std=c11", *common, *enum_flags, "-Wall", "-Wextra", "-c",
                     source, "-o", obj], env)
                objects.append(obj)
            exe = build / ("service-private-uplink-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags, *common,
                 HERE / "service_private_uplink_tests.cpp", app / "src/Protobuf.cpp",
                 vendor / "MtCodec.cpp", vendor / "MtSignature.cpp", *objects,
                 *host.crypto_link_flags(), "-o", exe], env)
            result = subprocess.run([str(exe)], check=False, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, env=env)
            print(label + ":", flush=True)
            print(result.stdout, end="")
            if result.returncode:
                raise SystemExit(result.returncode)

if __name__ == "__main__":
    main()
