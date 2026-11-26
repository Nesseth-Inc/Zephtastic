#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check the exact 32-bit permutation against independent FF1 known answers."""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[2]
sys.path.insert(0, str(root))
from host_test_toolchain import cc, cxx, crypto_compile_flags, crypto_link_flags

vendor = root / "zephcore/adapters/bench/vendor"
monocypher = root / "zephcore/lib/monocypher"
flags = ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
with tempfile.TemporaryDirectory() as folder:
    work = Path(folder)
    wipe = work / "monocypher.o"
    subprocess.run([*cc(), *flags, "-c", monocypher / "monocypher.c", "-o", wipe], check=True)
    backend = work / "codec.o"
    cpp = [*cxx(), "-std=c++17", *flags, *crypto_compile_flags(), "-I", vendor, "-I", monocypher]
    subprocess.run([*cpp, "-DmtAesCtr=systemAesCtr", "-c", vendor / "MtCodec.cpp", "-o", backend],
                   check=True)
    binary = work / "imported-nonce"
    subprocess.run([*cpp, here / "cases.cpp", backend, wipe, *crypto_link_flags(), "-o", binary],
                   check=True)
    subprocess.run([binary], check=True)
