#!/usr/bin/env python3
"""Default/short-enum ASan+UBSan fixtures for independent GPIO module settings."""
import argparse
import hashlib
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

if not __debug__:
    raise RuntimeError("optimized fixture runner refused")

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"
TEST = APP / "tests/m138_gpio_settings_tests.cpp"
SOURCES = [
    APP / "src/modules/GpioModuleSettings.h",
    APP / "src/modules/GpioModuleJournal.h",
    APP / "src/modules/GpioModuleEdit.h",
    APP / "src/SettingsJournal.h",
    APP / "src/Config.h",
    APP / "src/Service.h",
    APP / "src/Protobuf.cpp",
    TEST,
    PROTO / "module_config.pb.h",
    PROTO / "remote_hardware.pb.h",
    NANOPB / "pb.h",
    NANOPB / "pb_common.h",
    NANOPB / "pb_encode.h",
    NANOPB / "pb_decode.h",
    NANOPB / "pb_common.c",
    NANOPB / "pb_encode.c",
    NANOPB / "pb_decode.c",
    Path(__file__).resolve(),
]
EXPECTED_HEADERS = {
    "module_config.pb.h": "ca7cb612083dfd37143505728114756c813fe9e473ee457571c5bc601f0c1fa9",
    "remote_hardware.pb.h": "fd20927f917e92393d6ab11e6d60e9f23c5bda652301a04a3803ec2211eee881",
}

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def run(argv, env, log):
    log.write("$ " + shlex.join(map(str, argv)) + "\n")
    log.flush()
    result = subprocess.run(list(map(str, argv)), env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=120)
    log.write(result.stdout)
    log.flush()
    if result.returncode:
        raise RuntimeError("FIXTURE_COMMAND_FAILED:" + str(result.returncode))
    return result.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="fresh output directory outside the source worktree")
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists() or out.is_relative_to(ROOT):
        parser.error("output must be fresh and outside the source worktree")
    for name, expected in EXPECTED_HEADERS.items():
        if digest(PROTO / name) != expected:
            raise SystemExit("PINNED_STOCK_HEADER_MISMATCH:" + name)
    before = {str(path): digest(path) for path in SOURCES}
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    cc = shlex.split(env.get("CC", "clang"))
    cxx = shlex.split(env.get("CXX", "clang++"))
    out.mkdir(parents=True, exist_ok=False)
    (out / "meshtastic").mkdir()
    for header in PROTO.glob("*.pb.h"):
        shutil.copyfile(header, out / "meshtastic" / header.name)
    vendor = ROOT / "zephcore/adapters/bench/vendor"
    mono = ROOT / "zephcore/lib/monocypher"
    common_flags = ["-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I" + str(APP / "src"),
                    "-I" + str(APP / "tests"), "-I" + str(vendor),
                    "-I" + str(mono), "-I" + str(out),
                    "-I" + str(NANOPB)]
    executions = []
    try:
        for label, enum_flags in (("default", []), ("short-enums", ["-fshort-enums"])):
            folder = out / label
            folder.mkdir()
            objects = []
            with (folder / "build.log").open("x") as log:
                for index, source in enumerate((NANOPB / "pb_common.c",
                                                NANOPB / "pb_encode.c",
                                                NANOPB / "pb_decode.c")):
                    obj = folder / ("nanopb-" + str(index) + ".o")
                    run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                         *common_flags, *enum_flags, "-c", source, "-o", obj], env, log)
                    objects.append(obj)
                proto_obj = folder / "Protobuf.o"
                run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                     *common_flags, *enum_flags, "-c", APP / "src/Protobuf.cpp",
                     "-o", proto_obj], env, log)
                objects.append(proto_obj)
                test_obj = folder / "m138_gpio_settings_tests.o"
                run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                     *common_flags, *enum_flags, "-c", TEST, "-o", test_obj], env, log)
                binary = folder / "m138_gpio_settings_tests"
                run([*cxx, *common_flags, *enum_flags, test_obj, *objects,
                     "-o", binary], env, log)
                stdout = run([binary], env, log)
                executions.append({"variant": label, "status": "PASS",
                                   "pass_groups": sum(line.startswith("PASS ") for line in stdout.splitlines()),
                                   "binary_sha256": digest(binary),
                                   "log_sha256": digest(folder / "build.log")})
        after = {str(path): digest(path) for path in SOURCES}
        if before != after:
            raise RuntimeError("SOURCE_CHANGED_DURING_FIXTURES")
        import json
        result = {"schema": "zt-m138-gpio-settings-native-result/v1",
                  "status": "PASS", "stock_generated_headers": EXPECTED_HEADERS,
                  "source_inputs": before, "source_stable": True,
                  "executions": executions, "hardware": False,
                  "network": False}
        (out / "RESULT.json").write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
        print(json.dumps({"status": "PASS", "executions": len(executions),
                          "groups_per_variant": executions[0]["pass_groups"],
                          "ASAN_UBSAN": True, "hardware": False, "network": False}))
    except Exception:
        raise

if __name__ == "__main__":
    main()
