#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bounded real Service/Companion/nanopb export checks; no device I/O."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import sys

if not __debug__:
    raise SystemExit("Refusing disabled assertions")

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
SOURCE = APP.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--short-enums", action="store_true")
args = parser.parse_args()
output = args.output.resolve()
if output == SOURCE or SOURCE in output.parents:
    parser.error("output must be outside the source tree")
if output.exists():
    parser.error("refusing to overwrite prior evidence")

sys.dont_write_bytecode = True
helper = APP / "tests/airtime_quality/bounded_process.py"
sys.path.insert(0, str(helper.parent))
from bounded_process import run_group, install_termination_handler
install_termination_handler()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs():
    vendor = SOURCE / "zephcore/adapters/bench/vendor"
    proto = SOURCE / "zephcore/bench/upstream-differential/source"
    mono = SOURCE / "zephcore/lib/monocypher"
    files = list((APP / "src").rglob("*.h")) + list((APP / "src").rglob("*.hpp")) + [APP / "src/Protobuf.cpp"]
    files += list(vendor.glob("*.h")) + [vendor / "MtCodec.cpp", vendor / "MtSignature.cpp"]
    files += list(proto.glob("*.pb.h"))
    files += list((proto / "nanopb").glob("*.h"))
    files += [proto / "nanopb" / name for name in ("pb_common.c", "pb_encode.c", "pb_decode.c")]
    files += [mono / name for name in ("monocypher.h", "monocypher-ed25519.h",
                                      "monocypher.c", "monocypher-ed25519.c")]
    files += [HERE / "export_cases.cpp", Path(__file__).resolve(), helper]
    return {str(path.relative_to(SOURCE)): digest(path) for path in sorted(set(files))}


def tree_pins(root):
    return {str(path.relative_to(root)): digest(path)
            for path in sorted(root.rglob("*")) if path.is_file()}


def compiler(name, default):
    argv = shlex.split(os.environ.get(name, default))
    if not argv:
        parser.error(name + " cannot be empty")
    resolved = shutil.which(argv[0])
    if resolved is None:
        parser.error(name + " executable was not found")
    program = Path(resolved).resolve()
    return [str(program), *argv[1:]], {"executable": str(program),
                                     "sha256": digest(program), "argv": argv}


cc, cc_identity = compiler("CC", "cc")
cxx, cxx_identity = compiler("CXX", "c++")
original = source_inputs()
output.mkdir(parents=True)
receipt = {
    "status": "FAIL", "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "hardware": False, "radio_io": False, "network": False,
    "sanitizers": ["address", "undefined"], "optimization": "O2",
    "short_enums": args.short_enums, "source_sha256": original,
    "exports": {"paths": ["actual Companion admitted-frame node event", "actual Companion cached handshake",
                           "actual Companion local policy node event", "actual Companion MQTT-origin packet and cache provenance"],
                "fixture_limits": "deterministic radio/captured UTC; PrivateRuntime fakes prove admitted-event boundary only; no cryptographic, phone or physical RF proof"},
    "toolchain": {"CC": cc_identity, "CXX": cxx_identity}, "commands": [],
}
environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0",
               "UBSAN_OPTIONS": "halt_on_error=1", "PYTHONDONTWRITEBYTECODE": "1"}
frozen = output / "frozen-source"


def run(argv, log, timeout):
    entry = {"argv": [str(value) for value in argv], "timeout_seconds": timeout,
             "log": str(log.relative_to(output)), "completed": False}
    receipt["commands"].append(entry)
    try:
        with log.open("w") as stream:
            run_group(entry["argv"], timeout=timeout, stdout=stream, env=environment)
    except BaseException as error:
        if hasattr(error, "owned_process_cleanup"):
            entry["owned_process_cleanup"] = error.owned_process_cleanup
        raise
    entry.update(completed=True, leader_reaped=True, process_group_gone=True)


try:
    for relative, expected in original.items():
        target = frozen / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SOURCE / relative, target)
        if digest(target) != expected:
            raise RuntimeError("source changed during freeze: " + relative)
    if source_inputs() != original:
        raise RuntimeError("source changed during freeze")
    frozen_before = tree_pins(frozen)
    app = frozen / "mt_only"
    vendor = frozen / "zephcore/adapters/bench/vendor"
    proto = frozen / "zephcore/bench/upstream-differential/source"
    mono = frozen / "zephcore/lib/monocypher"
    build = output / "build"
    build.mkdir()
    (build / "meshtastic").mkdir()
    for header in proto.glob("*.pb.h"):
        shutil.copyfile(header, build / "meshtastic" / header.name)
    flags = ["-fsanitize=address,undefined", "-g", "-O2",
             "-I" + str(app / "src"), "-I" + str(vendor),
             "-I" + str(build), "-I" + str(proto / "nanopb"), "-I" + str(mono)]
    if sys.platform.startswith("linux"):
        flags += ["-DMT_HOST_OPENSSL=1"]
    if args.short_enums:
        flags += ["-fshort-enums"]
    objects = []
    c_sources = [proto / "nanopb" / name for name in ("pb_common.c", "pb_encode.c", "pb_decode.c")]
    c_sources += [mono / "monocypher.c", mono / "monocypher-ed25519.c"]
    for source in c_sources:
        obj = build / (source.name + ".o")
        run([*cc, "-std=c11", *flags, "-c", source, "-o", obj],
            build / (source.name + ".build.log"), 90)
        objects.append(obj)
    executable = build / "tests"
    link = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
            app / "tests/peer_last_heard/export_cases.cpp", app / "src/Protobuf.cpp",
            vendor / "MtCodec.cpp", vendor / "MtSignature.cpp", *objects]
    if sys.platform.startswith("linux"):
        link += ["-lcrypto"]
    link += ["-o", executable]
    run(link, build / "build.log", 120)
    run([executable], build / "test.log", 30)
    lines = (build / "test.log").read_text().splitlines()
    groups = [line for line in lines if line.startswith("PASS ")]
    if len(groups) != 37 or lines[-1:] != ["EXPORT_GROUPS 37"]:
        raise RuntimeError("missing independent export acceptance cases")
    receipt["cases"] = {"groups": groups, "count": len(groups),
                        "executable_sha256": digest(executable)}
    receipt["frozen_inputs_unchanged"] = tree_pins(frozen) == frozen_before
    receipt["source_unchanged"] = source_inputs() == original
    receipt["toolchain_unchanged"] = all(
        digest(Path(identity["executable"])) == identity["sha256"]
        for identity in receipt["toolchain"].values())
    if not all(receipt[key] for key in ("frozen_inputs_unchanged", "source_unchanged", "toolchain_unchanged")):
        raise RuntimeError("input identity changed")
    receipt["status"] = "PASS"
except BaseException as error:
    receipt["error"] = {"type": type(error).__name__, "detail": str(error)}
    if hasattr(error, "owned_process_cleanup"):
        receipt["error"]["owned_process_cleanup"] = error.owned_process_cleanup
finally:
    receipt["ended_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (output / "SUMMARY.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps({key: value for key, value in receipt.items() if key not in ("source_sha256", "commands")}))
raise SystemExit(receipt["status"] != "PASS")
