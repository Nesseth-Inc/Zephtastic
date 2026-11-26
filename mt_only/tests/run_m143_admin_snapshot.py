#!/usr/bin/env python3
"""Build/run synthetic Companion Admin snapshot tests with pinned host PSA crypto."""
import argparse
import hashlib
import json
import os
import platform
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
for name in ("source", "output", "psa-root", "psa-library"):
    p.add_argument("--" + name, type=Path, required=True)
a = p.parse_args()
root = a.source.resolve()
out = a.output.resolve()
if out.exists() or out.is_relative_to(root):
    p.error("output must be fresh and outside the source tree")
app = root / "mt_only"
test = app / "tests/m143_admin_snapshot_tests.cpp"
proto = root / "zephcore/bench/upstream-differential/source"
nanopb = proto / "nanopb"
vendor = root / "zephcore/adapters/bench/vendor"
mono = root / "zephcore/lib/monocypher"
psa = a.psa_root.resolve()
library = a.psa_library.resolve()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def snapshot():
    paths = [test, app / "tests/pki_runtime_fixture.h", app / "tests/known_channel_fixture.h",
             app / "src/Companion.h", app / "src/Service.h", app / "src/Endpoint.h",
             app / "src/Config.h", app / "src/HardwareSnapshot.h", app / "src/Protobuf.cpp",
             app / "src/admin/RemoteAdminProtocol.h", app / "src/admin/RemoteAdminRedaction.h",
             app / "src/admin/CompanionWorkspace.h",
             app / "src/pki/PkiRuntime.h", app / "src/pki/PkiLedger.h",
             app / "src/pki/PsaPkiBackend.h", app / "experimental/pki/PkiCrypto.h",
             app / "tests/pki/psa-host-config.h", app / "dependency-lock.json",
             vendor / "MtCodec.h", vendor / "MtCodec.cpp", vendor / "MtSignature.h",
             vendor / "MtSignature.cpp", mono / "monocypher.h", mono / "monocypher-ed25519.h",
             mono / "monocypher.c", mono / "monocypher-ed25519.c",
             *(proto.glob("*.pb.h")),
             *(nanopb / name for name in ("pb.h", "pb_common.h", "pb_encode.h", "pb_decode.h",
                                          "pb_common.c", "pb_encode.c", "pb_decode.c"))]
    return {str(path.relative_to(root)): sha(path) for path in sorted(set(paths))}

before = snapshot()
out.mkdir(parents=True)
generated = out / "generated" / "meshtastic"
generated.mkdir(parents=True)
for header in proto.glob("*.pb.h"):
    (generated / header.name).write_bytes(header.read_bytes())

includes = [app / "src", app / "tests", app / "src/pki", app / "experimental/pki",
            vendor, mono, out / "generated", nanopb, psa / "include",
            psa / "drivers/builtin/include", psa / "dispatch/include",
            library.parent.parent / "include"]
flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-DCONFIG_MT_RF_ADMIN_GET=1",
         *["-I" + str(path) for path in includes],
         '-DTF_PSA_CRYPTO_CONFIG_FILE="' + str(app / "tests/pki/psa-host-config.h") + '"']
if platform.system() == "Linux":
    flags.append("-DMT_HOST_OPENSSL=1")
cc = shlex.split(os.environ.get("CC", "clang"))
cxx = shlex.split(os.environ.get("CXX", "clang++"))
host_link = ["-lcrypto"] if platform.system() == "Linux" else []
commands = []

def run(argv, log):
    argv = list(map(str, argv))
    commands.append(argv)
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180)
    log.write("$ " + shlex.join(argv) + "\n" + result.stdout)
    log.flush()
    if result.returncode:
        raise RuntimeError("COMMAND_FAILED:" + str(result.returncode))
    return result.stdout

results = []
cpp_sources = [app / "src/Protobuf.cpp", vendor / "MtCodec.cpp", vendor / "MtSignature.cpp",
               *(app / "src/pki" / (name + ".cpp") for name in
                 ("PkiRuntime", "PkiSession", "PkiIdentity", "PkiLedger", "PsaPkiBackend",
                  "PkiSigner", "PkiStorage", "PkiAppendHistory")),
               app / "experimental/pki/PkiCrypto.cpp", app / "experimental/pki/PkiAck.cpp"]
c_sources = [nanopb / name for name in ("pb_common.c", "pb_encode.c", "pb_decode.c")]
c_sources += [mono / "monocypher.c", mono / "monocypher-ed25519.c"]
for label, extra in (("default-enums", []), ("short-enums", ["-fshort-enums"])):
    folder = out / label
    folder.mkdir()
    with (folder / "build.log").open("x") as log:
        objects = []
        for i, source in enumerate(c_sources):
            obj = folder / ("c-" + str(i) + ".o")
            run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, *extra,
                 "-c", source, "-o", obj], log)
            objects.append(obj)
        for i, source in enumerate([*cpp_sources, test]):
            obj = folder / ("cpp-" + str(i) + ".o")
            run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags, *extra,
                 "-c", source, "-o", obj], log)
            objects.append(obj)
        binary = folder / "snapshot-tests"
        run([*cxx, *flags, *extra, *objects, library, *host_link, "-o", binary], log)
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1"}
        completed = subprocess.run([binary], capture_output=True, text=True,
                                   timeout=60, env=env)
        (folder / "test.log").write_text(completed.stdout + completed.stderr)
        results.append({"variant": label, "status": "PASS" if completed.returncode == 0 else "RUN_FAIL",
                        "returncode": completed.returncode, "stdout": completed.stdout.strip(),
                        "stderr_tail": completed.stderr[-2000:], "binary_sha256": sha(binary),
                        "build_log_sha256": sha(folder / "build.log")})

after = snapshot()
stable = before == after
result = {"schema": "m143-admin-snapshot-native/v1",
          "status": "PASS" if stable and all(x["status"] == "PASS" for x in results) else "FAIL",
          "source_stable": stable, "source_sha256": before,
          "psa_library_sha256": sha(library), "variants": results,
          "asan_ubsan": True, "hardware": False, "rf": False,
          "evidence_scope": "Direct synthetic Companion trusted-snapshot callback only; does not prove auth provenance, encryption, RF delivery, or target stack use.",
          "commands": commands}
(out / "RESULT.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
print(json.dumps({"status": result["status"], "variants": results}, sort_keys=True))
raise SystemExit(0 if result["status"] == "PASS" else 1)
