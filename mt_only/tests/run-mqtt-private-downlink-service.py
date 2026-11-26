#!/usr/bin/env python3
"""Run the native M123 Service/Companion private MQTT receive fixture."""
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host

APP = ROOT / "mt_only"
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"
VENDOR = ROOT / "zephcore/adapters/bench/vendor"
MONO = ROOT / "zephcore/lib/monocypher"


def run(command, env):
    print("$", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, env=env)


def main():
    service_text = (APP / "src/Service.h").read_text()
    companion_text = (APP / "src/Companion.h").read_text()
    for token in ("bindMqttLocalRxSink", "mqttDownlinkFrame", "privateRuntimeReady",
                  "pinnedPrivatePeer", "anyChannelDownlinkEnabled"):
        if token not in service_text:
            raise SystemExit("Service source lacks expected M123 private MQTT receive path")
    if "out.via_mqtt=(p[12]&0x10u)!=0" not in companion_text:
        raise SystemExit("Companion source lacks expected MQTT-origin private metadata mapping")
    if not all(path.is_dir() for path in (PROTO, NANOPB, VENDOR, MONO)):
        raise SystemExit("source checkout is missing native-test dependencies")

    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="m123-private-downlink-service-") as tmp:
        build = Path(tmp)
        generated = build / "meshtastic"
        generated.mkdir()
        for header in PROTO.glob("*.pb.h"):
            shutil.copyfile(header, generated / header.name)
        common = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
                  "-DPB_ENABLE_MALLOC", "-DCONFIG_MT_MQTT_GATEWAY=1",
                  "-I" + str(build), "-I" + str(NANOPB), "-I" + str(APP / "src"),
                  "-I" + str(APP / "tests"), "-I" + str(VENDOR), "-I" + str(MONO),
                  *host.crypto_compile_flags()]
        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1"
        c_sources = (NANOPB / "pb_common.c", NANOPB / "pb_encode.c", NANOPB / "pb_decode.c",
                     MONO / "monocypher.c", MONO / "monocypher-ed25519.c")
        for label, enum_flags in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
            objects = []
            for source in c_sources:
                obj = build / (label + "-" + source.stem + ".o")
                run([*cc, "-std=c11", *common, *enum_flags, "-Wall", "-Wextra", "-c",
                     source, "-o", obj], env)
                objects.append(obj)
            exe = build / ("mqtt-private-downlink-service-" + label)
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *enum_flags, *common,
                 APP / "tests/mqtt_private_downlink_service_tests.cpp", APP / "src/Protobuf.cpp",
                 VENDOR / "MtCodec.cpp", VENDOR / "MtSignature.cpp", *objects,
                 *host.crypto_link_flags(), "-o", exe], env)
            result = subprocess.run([str(exe)], check=False, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
            print(label + ":", flush=True)
            print(result.stdout, end="")
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
