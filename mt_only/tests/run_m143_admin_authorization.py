#!/usr/bin/env python3
"""Compile and run only the synthetic M143 RF Admin authorization fixture."""
from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

TESTS = Path(__file__).resolve().parent
SRC = TESTS.parent / "src"
WT = SRC.parents[1]
GENERATED = WT / "zephcore/bench/upstream-differential/source"
NANOPB = GENERATED / "nanopb"
VENDOR = WT / "zephcore/adapters/bench/vendor"


def main() -> int:
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    if not compiler or not shutil.which(compiler[0]):
        raise SystemExit("No host C++ compiler found")
    version = subprocess.run(compiler + ["--version"], capture_output=True,
                             text=True, timeout=10)
    compiler_version = ((version.stdout or version.stderr).splitlines() or ["unknown"])[0]
    results = []

    with tempfile.TemporaryDirectory(prefix="m143-admin-auth-") as temporary:
        temp = Path(temporary)
        generated_package = temp / "meshtastic"
        generated_package.mkdir()
        for header in GENERATED.glob("*.pb.h"):
            (generated_package / header.name).symlink_to(header)
        includes = [f"-I{temp}", f"-I{SRC}", f"-I{GENERATED}",
                    f"-I{NANOPB}", f"-I{VENDOR}"]
        source = TESTS / "m143_admin_authorization_tests.cpp"
        modes = [
            ("plain-enums", []),
            ("short-enums", ["-fshort-enums"]),
            ("asan-ubsan", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
        ]
        for label, extra in modes:
            executable = temp / f"admin-auth-{label}"
            command = [*compiler, "-std=c++17", "-O1", *extra, *includes,
                       str(source), "-o", str(executable)]
            built = subprocess.run(command, capture_output=True, text=True)
            if built.returncode:
                results.append({"mode": label, "status": "COMPILE_FAIL",
                                "returncode": built.returncode,
                                "stderr_tail": built.stderr[-2000:]})
                continue
            ran = subprocess.run([str(executable)], capture_output=True,
                                 text=True, timeout=30)
            results.append({"mode": label,
                            "status": "PASS" if ran.returncode == 0 else "RUN_FAIL",
                            "returncode": ran.returncode,
                            "stdout": ran.stdout.strip(),
                            "stderr_tail": ran.stderr[-1000:]})

    print(json.dumps({"compiler": compiler[0], "compiler_version": compiler_version,
                      "results": results}, sort_keys=True))
    return 0 if len(results) == 3 and all(r["status"] == "PASS" for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
