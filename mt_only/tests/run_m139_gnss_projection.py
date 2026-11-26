#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Default/short-enum ASan/UBSan tests for native GNSS heading projection."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
WT = APP.parent
TEST = APP / "tests/m139_gnss_projection_tests.cpp"
OVERLAY = APP / "dependency-overlay/zephyr/drivers/gnss"
SDK_FIXTURE = APP / "tests/gnss_native/sdk"
PROVENANCE = APP / "tests/gnss_native/PROVENANCE.json"
SOURCES = [TEST, OVERLAY / "gnss_nmea0183.c", OVERLAY / "gnss_nmea0183_match.c",
           SDK_FIXTURE / "drivers/gnss/gnss_parse.c"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, env, log):
    log.write("$ " + shlex.join(map(str, argv)) + "\n")
    log.flush()
    result = subprocess.run(list(map(str, argv)), cwd=WT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=180)
    log.write(result.stdout)
    log.flush()
    if result.returncode:
        raise RuntimeError("NATIVE_GNSS_COMMAND_FAILED:" + str(result.returncode))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="fresh result directory outside the source worktree")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or output.is_relative_to(WT):
        parser.error("output must be fresh and outside the source worktree")

    provenance = json.loads(PROVENANCE.read_text())
    if provenance["SDK_own_git_identity_claimed"] or not provenance["kernel_runtime_modeled"]:
        parser.error("unexpected GNSS SDK provenance boundary")
    for name, row in provenance["files"].items():
        path = SDK_FIXTURE / name
        if path.stat().st_size != row["bytes"] or digest(path) != row["sha256"]:
            parser.error("pinned SDK fixture mismatch: " + name)

    inputs = [TEST, *SOURCES[1:], PROVENANCE,
              *[SDK_FIXTURE / name for name in provenance["files"]]]
    before = {str(path): digest(path) for path in inputs}
    output.mkdir(parents=True)
    stub = output / "stubs"
    (stub / "zephyr/drivers/gnss").mkdir(parents=True)
    (stub / "zephyr/modem").mkdir(parents=True)
    (stub / "zephyr/data").mkdir(parents=True)
    (stub / "zephyr/kernel.h").write_text("""#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define __ASSERT(c,...) assert(c)
#define BIT(x) (1u<<(x))
#define ARG_UNUSED(x) (void)(x)
#define snprintk snprintf
#define vsnprintk vsnprintf
""")
    (stub / "zephyr/types.h").write_text("#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n")
    (stub / "zephyr/device.h").write_text("#pragma once\nstruct device {int unused;};\n")
    (stub / "zephyr/modem/chat.h").write_text("#pragma once\nstruct modem_chat {int unused;};\n")
    header = (SDK_FIXTURE / "include/zephyr/drivers/gnss.h").read_text()
    names = ["enum gnss_system", "enum gnss_fix_status", "enum gnss_fix_quality",
             "struct gnss_info", "struct gnss_time", "struct gnss_data",
             "struct gnss_satellite"]
    import re
    declarations = []
    for name in names:
        match = re.search(re.escape(name) + r"\s*\{.*?\n\};", header, re.S)
        if not match:
            parser.error("SDK GNSS declaration not found: " + name)
        declarations.append(match.group())
    (stub / "zephyr/drivers/gnss.h").write_text(
        "#pragma once\n#include <zephyr/kernel.h>\n#include <zephyr/device.h>\n"
        "#include <zephyr/data/navigation.h>\n" + "\n".join(declarations) + "\n")
    (stub / "zephyr/data/navigation.h").write_bytes(
        (SDK_FIXTURE / "include/zephyr/data/navigation.h").read_bytes())
    (stub / "zephyr/drivers/gnss/gnss_publish.h").write_text(
        "#pragma once\n#include <zephyr/drivers/gnss.h>\n"
        "void gnss_publish_data(const struct device *,const struct gnss_data *);\n")
    (stub / "meshtastic").symlink_to(APP.parent / "zephcore/bench/upstream-differential/source",
                                      target_is_directory=True)

    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1"
    cxx = shlex.split(env.get("CXX", "clang++"))
    cc = shlex.split(env.get("CC", "clang"))
    common = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
              "-I", str(stub), "-I", str(SDK_FIXTURE / "drivers"),
              "-I", str(SDK_FIXTURE / "drivers/gnss"),
              "-I", str(APP / "src"),
              "-I", str(APP.parent / "zephcore/bench/upstream-differential/source/nanopb")]
    runs = []
    for label, enum_flags in (("default", []), ("short-enums", ["-fshort-enums"])):
        folder = output / label
        folder.mkdir()
        objects = []
        with (folder / "build.log").open("x") as log:
            for index, source in enumerate((OVERLAY / "gnss_nmea0183.c",
                                            OVERLAY / "gnss_nmea0183_match.c",
                                            SDK_FIXTURE / "drivers/gnss/gnss_parse.c")):
                obj = folder / f"gnss-{index}.o"
                run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-pointer-sign",
                     *common, *enum_flags, "-c", source, "-o", obj], env, log)
                objects.append(str(obj))
            test_obj = folder / "projection.o"
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *common,
                 *enum_flags, "-c", TEST, "-o", test_obj], env, log)
            objects.append(str(test_obj))
            binary = folder / "m139_gnss_projection_tests"
            run([*cxx, *common, *enum_flags, *objects, "-o", binary], env, log)
            stdout = run([binary], env, log)
            runs.append({"variant": label, "status": "PASS", "stdout": stdout.strip(),
                         "binary_sha256": digest(binary),
                         "log_sha256": digest(folder / "build.log")})

    after = {str(path): digest(path) for path in inputs}
    if before != after:
        raise RuntimeError("SOURCE_CHANGED_DURING_NATIVE_GNSS_TEST")
    result = {
        "schema": "zt-m139-native-gnss-projection/v1",
        "status": "PASS",
        "sdk_header_fixture_sha256": digest(SDK_FIXTURE / "include/zephyr/drivers/gnss.h"),
        "actual_parser_and_matcher_sources": [str(path.relative_to(APP)) for path in SOURCES[1:3]],
        "SDK_parser_helpers": True,
        "kernel_runtime_modeled": True,
        "source_stable": True,
        "inputs_sha256": before,
        "runs": runs,
        "ASAN_UBSAN": True,
        "hardware": False,
        "radio": False,
        "network": False,
        "target_build": False,
    }
    (output / "RESULT.json").write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "variants": len(runs), "source_stable": True}))


if __name__ == "__main__":
    main()
