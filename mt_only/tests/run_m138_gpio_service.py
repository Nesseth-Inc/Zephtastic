#!/usr/bin/env python3
"""Build/run actual Service + GpioModuleService + Companion native fixtures."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"
VENDOR = ROOT / "zephcore/adapters/bench/vendor"
MONO = ROOT / "zephcore/lib/monocypher"
TEST = APP / "tests/m138_gpio_service_tests.cpp"
RUNNER = Path(__file__).resolve()
INPUTS = [
    TEST, RUNNER, APP / "tests/endpoint_fixture.h", APP / "tests/known_channel_fixture.h",
    APP / "src/Service.h", APP / "src/Companion.h", APP / "src/Endpoint.h",
    APP / "src/Config.h", APP / "src/ChannelRecord.h", APP / "src/PrimaryChannel.hpp",
    APP / "src/modules/GpioModuleService.h", APP / "src/modules/GpioModuleWire.h",
    APP / "src/modules/GpioModuleChannels.h", APP / "src/modules/GpioModuleRuntime.h",
    APP / "src/modules/GpioModuleBackend.h", APP / "src/modules/GpioModuleSettings.h",
    APP / "src/modules/GpioModuleEdit.h", APP / "src/modules/GpioModuleJournal.h",
    APP / "src/modules/RemoteHardwarePolicy.h", APP / "src/modules/DetectionSensorPolicy.h",
    APP / "src/SettingsJournal.h", APP / "src/Protobuf.cpp",
    VENDOR / "MtCodec.h", VENDOR / "MtCodec.cpp", VENDOR / "MtSignature.h",
    VENDOR / "MtSignature.cpp", VENDOR / "MtNodeInfo.h", VENDOR / "MtKeyCommand.h",
    MONO / "monocypher.c", MONO / "monocypher-ed25519.c",
    *[PROTO / name for name in ("mesh.pb.h", "admin.pb.h", "module_config.pb.h",
        "remote_hardware.pb.h", "portnums.pb.h")],
    *[NANOPB / name for name in ("pb.h", "pb_common.h", "pb_encode.h", "pb_decode.h",
        "pb_common.c", "pb_encode.c", "pb_decode.c")],
]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(command, env, log):
    log.write("$ " + shlex.join(map(str, command)) + "\n")
    log.flush()
    result = subprocess.run(list(map(str, command)), env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=180)
    log.write(result.stdout)
    log.flush()
    if result.returncode:
        raise RuntimeError("FIXTURE_COMMAND_FAILED:" + str(result.returncode))
    return result.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or output.is_relative_to(ROOT):
        parser.error("output must be fresh and outside source root")
    before = {str(path): sha(path) for path in INPUTS}
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    cc = shlex.split(env.get("CC", "clang"))
    cxx = shlex.split(env.get("CXX", "clang++"))
    host_link = []
    common = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-DCONFIG_MT_GPIO_MODULES=1",
              "-I" + str(APP / "src"), "-I" + str(APP / "tests"),
              "-I" + str(VENDOR), "-I" + str(MONO), "-I" + str(output),
              "-I" + str(NANOPB)]
    if sys.platform.startswith("linux"):
        common += ["-DMT_HOST_OPENSSL=1"]
        host_link += ["-lcrypto"]
    output.mkdir(parents=True)
    generated = output / "meshtastic"
    generated.mkdir()
    for header in PROTO.glob("*.pb.h"):
        shutil.copyfile(header, generated / header.name)
    variants = []
    for label, enum_flags in (("default", []), ("short-enums", ["-fshort-enums"])):
        folder = output / label
        folder.mkdir()
        objects = []
        with (folder / "build.log").open("x") as log:
            for i, source in enumerate((NANOPB / "pb_common.c", NANOPB / "pb_encode.c",
                                        NANOPB / "pb_decode.c", MONO / "monocypher.c",
                                        MONO / "monocypher-ed25519.c")):
                obj = folder / ("c-" + str(i) + ".o")
                run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                     *common, *enum_flags, "-c", source, "-o", obj], env, log)
                objects.append(obj)
            for name, source, extra in (
                ("proto", APP / "src/Protobuf.cpp", []),
                ("codec", VENDOR / "MtCodec.cpp", []),
                ("signature", VENDOR / "MtSignature.cpp", []),
                ("fixture", TEST, [])):
                obj = folder / (name + ".o")
                unused_fixture_helpers = ["-Wno-unused-function"] if name == "fixture" else []
                run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                     *unused_fixture_helpers, *common, *enum_flags, *extra, "-c", source, "-o", obj], env, log)
                objects.append(obj)
            binary = folder / "m138_gpio_service_tests"
            run([*cxx, *common, *enum_flags, *objects, *host_link, "-o", binary], env, log)
            stdout = run([binary], env, log)
            variants.append({
                "variant": label,
                "pass_groups": sum(line.startswith("PASS ") for line in stdout.splitlines()),
                "binary_sha256": sha(binary),
                "log_sha256": sha(folder / "build.log"),
            })
    after = {str(path): sha(path) for path in INPUTS}
    if before != after:
        raise RuntimeError("SOURCE_CHANGED_DURING_FIXTURE")
    result = {
        "schema": "zt-m138-gpio-service-native-result/v1",
        "status": "PASS",
        "source_stable": True,
        "source_sha256": before,
        "executions": variants,
        "ASAN_UBSAN": True,
        "GPIO_hardware": False,
        "RF": False,
        "network": False,
    }
    (output / "RESULT.json").write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "executions": len(variants),
                      "groups_per_variant": variants[0]["pass_groups"],
                      "ASAN_UBSAN": True, "GPIO_hardware": False, "RF": False}))

if __name__ == "__main__":
    main()
