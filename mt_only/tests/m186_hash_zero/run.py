#!/usr/bin/env python3
"""Build and run the focused hash-zero host fixture; Root owns execution."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys

if not __debug__:
    raise SystemExit("Refusing disabled assertions")

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", required=True, type=Path, help="Frozen source checkout")
parser.add_argument("--expected-head", required=True, help="Reviewed exact source commit")
parser.add_argument("--output", required=True, type=Path, help="New evidence directory outside source")
args = parser.parse_args()
SOURCE = args.source.resolve()
APP = SOURCE / "mt_only"
output = args.output.resolve()
if output == SOURCE or SOURCE in output.parents:
    parser.error("output must be outside the source tree")
if output.exists():
    parser.error("refusing to overwrite prior evidence")
if not re.fullmatch(r"[0-9a-fA-F]{40}", args.expected_head):
    parser.error("--expected-head must be a full 40-character commit ID")

manifest_path = SOURCE.parent / "SOURCE-MANIFEST.json"
manifest_hash = None
manifest_files = {}
if (SOURCE / ".git").exists():
    source_kind = "git"
    head = subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(SOURCE), "status", "--porcelain=v1", "--untracked-files=all"], text=True)
    if dirty:
        parser.error("refusing dirty source checkout")
else:
    source_kind = "exported-manifest"
    try:
        manifest_bytes = manifest_path.read_bytes()
        manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
        manifest = json.loads(manifest_bytes)
    except (OSError, json.JSONDecodeError) as error:
        parser.error("export requires sibling SOURCE-MANIFEST.json: " + str(error))
    head = manifest.get("source_revision") if isinstance(manifest, dict) else None
    entries = manifest.get("files") if isinstance(manifest, dict) else None
    if not isinstance(head, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", head) or not isinstance(entries, list):
        parser.error("source manifest must contain a full source_revision and files array")
    for item in entries:
        if not isinstance(item, dict):
            parser.error("source manifest entry must be an object")
        raw_path, expected_digest = item.get("path"), item.get("public_sha256")
        if not isinstance(raw_path, str) or not raw_path or "\\" in raw_path or "\0" in raw_path:
            parser.error("source manifest contains an unsafe path")
        relative = PurePosixPath(raw_path)
        if relative.is_absolute() or relative.as_posix() != raw_path or any(part in ("", ".", "..") for part in relative.parts):
            parser.error("source manifest contains a non-normalized path: " + raw_path)
        if raw_path in manifest_files:
            parser.error("source manifest contains a duplicate path: " + raw_path)
        if not isinstance(expected_digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected_digest):
            parser.error("source manifest contains an invalid SHA-256: " + raw_path)
        resolved = (SOURCE / raw_path).resolve()
        if not resolved.is_relative_to(SOURCE) or not resolved.is_file():
            parser.error("source manifest path is missing or escapes source root: " + raw_path)
        if hashlib.sha256(resolved.read_bytes()).hexdigest().lower() != expected_digest.lower():
            parser.error("source manifest file is missing or mismatched: " + raw_path)
        manifest_files[raw_path] = expected_digest.lower()
    if {p.relative_to(SOURCE).as_posix() for p in SOURCE.rglob('*') if p.is_file()} != set(manifest_files):
        parser.error("export inventory differs from source manifest")
if head.lower() != args.expected_head.lower():
    parser.error("source revision differs from --expected-head")

sys.dont_write_bytecode = True

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def source_inputs():
    files = list((APP / "src").rglob("*.h")) + list((APP / "src").rglob("*.hpp"))
    vendor = SOURCE / "zephcore/adapters/bench/vendor"
    proto = SOURCE / "zephcore/bench/upstream-differential/source"
    mono = SOURCE / "zephcore/lib/monocypher"
    files += list(vendor.glob("*.h"))
    files += [vendor / name for name in ("MtCodec.cpp", "MtSignature.cpp")]
    files += list(proto.glob("*.pb.h"))
    files += list((proto / "nanopb").glob("*.h")) + list((proto / "nanopb").glob("*.c"))
    files += [APP / "src/Protobuf.cpp", mono / "monocypher.h", mono / "monocypher-ed25519.h",
              mono / "monocypher.c", mono / "monocypher-ed25519.c",
              APP / "tests/known_channel_fixture.h"]
    return {str(path.relative_to(SOURCE)): digest(path) for path in sorted(set(files))}

def compiler(name, default):
    argv = shlex.split(os.environ.get(name, default))
    if not argv:
        parser.error(name + " cannot be empty")
    resolved = shutil.which(argv[0])
    if resolved is None:
        parser.error(name + " executable was not found")
    # Keep argv[0]'s driver spelling. On Linux clang++ is commonly a symlink
    # to clang; resolving it selects the C driver and omits C++ ABI libraries
    # at link time. Hash the resolved executable while recording both paths.
    invocation = Path(resolved).absolute()
    program = invocation.resolve(strict=True)
    return [str(invocation), *argv[1:]], {
        "executable": str(program), "invocation": str(invocation),
        "sha256": digest(program), "argv": argv,
    }

cc, cc_identity = compiler("CC", "cc")
cxx, cxx_identity = compiler("CXX", "c++")
original = source_inputs()
if source_kind == "exported-manifest":
    selected = {str(Path(name).as_posix()) for name in original}
    if not selected.issubset(manifest_files):
        parser.error("source manifest omits a required fixture input")
test_identity = {name: digest(HERE / name) for name in ("cases.cpp", "run.py", "README.md")}
output.mkdir(parents=True)
frozen = output / "frozen-source"
build = output / "host"
build.mkdir()
(build / "meshtastic").mkdir()
receipt = {
    "status": "FAIL", "source_head": head,
    "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "hardware": False, "radio_io": False, "network": False,
    "sanitizers": ["address", "undefined"], "optimization": "O1",
    "scope": "actual Config/PrimaryChannel/MtCodec/Service/Companion/Endpoint/RoutingCore host fixtures; synthetic radio and PKI classification probe",
    "source_kind": source_kind, "source_manifest_sha256": manifest_hash,
    "source_sha256": original, "test_sha256": test_identity,
    "toolchain": {"CC": cc_identity, "CXX": cxx_identity}, "commands": []}
env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1",
       "PYTHONDONTWRITEBYTECODE": "1"}

def run(argv, log, timeout):
    entry = {"argv": [str(value) for value in argv], "log": str(log.relative_to(output)),
             "timeout_seconds": timeout, "completed": False}
    receipt["commands"].append(entry)
    with log.open("w") as stream:
        subprocess.run(entry["argv"], check=True, timeout=timeout, env=env,
                       stdout=stream, stderr=subprocess.STDOUT)
    entry["completed"] = True

try:
    for relative, expected in original.items():
        target = frozen / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SOURCE / relative, target)
        if digest(target) != expected:
            raise RuntimeError("source changed while freezing: " + relative)
    if source_inputs() != original:
        raise RuntimeError("source changed after freezing")
    frozen_app = frozen / "mt_only"
    vendor = frozen / "zephcore/adapters/bench/vendor"
    proto = frozen / "zephcore/bench/upstream-differential/source"
    mono = frozen / "zephcore/lib/monocypher"
    for header in proto.glob("*.pb.h"):
        shutil.copyfile(header, build / "meshtastic" / header.name)
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-O1",
             "-I" + str(frozen_app / "src"), "-I" + str(frozen_app / "tests"),
             "-I" + str(vendor), "-I" + str(build), "-I" + str(proto / "nanopb"),
             "-I" + str(mono)]
    if sys.platform.startswith("linux"):
        flags += ["-DMT_HOST_OPENSSL=1"]
    c_objects = []
    for source in [proto / "nanopb" / leaf for leaf in ("pb_common.c", "pb_encode.c", "pb_decode.c")]:
        obj = build / (source.name + ".o")
        run([*cc, "-std=c11", *flags, "-c", source, "-o", obj], build / (source.name + ".log"), 90)
        c_objects.append(obj)
    for source in (mono / "monocypher.c", mono / "monocypher-ed25519.c"):
        obj = build / (source.name + ".o")
        run([*cc, "-std=c11", *flags, "-c", source, "-o", obj], build / (source.name + ".log"), 90)
        c_objects.append(obj)
    executable = build / "hash-zero-cases"
    # Keep the authored test alongside the frozen inputs rather than reaching
    # back into a mutable workspace during compilation.
    case_copy = frozen / "m186/hash-zero-tests/cases.cpp"
    case_copy.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(HERE / "cases.cpp", case_copy)
    if digest(case_copy) != test_identity["cases.cpp"]:
        raise RuntimeError("authored fixture changed while freezing")
    argv = [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags, case_copy]
    argv += [frozen_app / "src/Protobuf.cpp", vendor / "MtCodec.cpp", vendor / "MtSignature.cpp", *c_objects]
    if sys.platform.startswith("linux"):
        argv += ["-lcrypto"]
    # macOS CommonCrypto symbols are provided by the default system library,
    # as in the source-owned host runners; there is no standalone framework.
    argv += ["-o", executable]
    run(argv, build / "build.log", 180)
    run([executable], build / "cases.log", 60)
    cases = [line for line in (build / "cases.log").read_text().splitlines() if line.startswith("PASS ")]
    if len(cases) != 6:
        raise RuntimeError("expected six named acceptance groups")
    receipt["cases"] = cases
    if source_kind == "git":
        current_head = subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip()
        current_dirty = subprocess.check_output(["git", "-C", str(SOURCE), "status", "--porcelain=v1", "--untracked-files=all"], text=True)
        source_unchanged = current_head.lower() == args.expected_head.lower() and not current_dirty
    else:
        current_manifest = manifest_path.read_bytes()
        source_unchanged = hashlib.sha256(current_manifest).hexdigest() == manifest_hash
        source_unchanged = source_unchanged and {p.relative_to(SOURCE).as_posix() for p in SOURCE.rglob('*') if p.is_file()} == set(manifest_files)
        source_unchanged = source_unchanged and all(
            hashlib.sha256((SOURCE / name).resolve().read_bytes()).hexdigest().lower() == expected
            for name, expected in manifest_files.items())
    receipt["source_unchanged"] = source_unchanged and source_inputs() == original
    receipt["test_unchanged"] = {name: digest(HERE / name) for name in test_identity} == test_identity
    if not receipt["source_unchanged"] or not receipt["test_unchanged"]:
        raise RuntimeError("source changed during host execution")
    receipt["status"] = "PASS"
except BaseException as error:
    receipt["error"] = {"type": type(error).__name__, "detail": str(error)}
finally:
    receipt["ended_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (output / "SUMMARY.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps({key: value for key, value in receipt.items()
                  if key not in ("source_sha256", "commands")}))
raise SystemExit(receipt["status"] != "PASS")
