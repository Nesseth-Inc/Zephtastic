#!/usr/bin/env python3
"""Portable, host-only compiler/link policy for native regression runners.

Target builds never import this module. Linux native tests use the caller's
CC/CXX and OpenSSL; macOS retains system CommonCrypto and Mach-O dead stripping.
"""
import os
import shlex
import shutil
import sys


def cc():
    return shlex.split(os.environ.get("CC", "cc"))


def cxx():
    return shlex.split(os.environ.get("CXX", "c++"))


def crypto_compile_flags():
    if sys.platform.startswith("linux"):
        return ["-DMT_HOST_OPENSSL=1"]
    if sys.platform == "darwin":
        return []
    raise RuntimeError(f"No reviewed host AES backend for {sys.platform}")


def crypto_link_flags():
    if sys.platform.startswith("linux"):
        return ["-lcrypto"]
    if sys.platform == "darwin":
        return []
    raise RuntimeError(f"No reviewed host AES backend for {sys.platform}")


def dead_strip_flag():
    if sys.platform.startswith("linux"):
        return "-Wl,--gc-sections"
    if sys.platform == "darwin":
        return "-Wl,-dead_strip"
    raise RuntimeError(f"No reviewed section-GC linker flag for {sys.platform}")


def position_policy_link(app, output):
    """Link the real channel policy into settings-only tests, without a packet codec."""
    proto = app.parent / "zephcore/bench/upstream-differential/source"
    headers = output / "meshtastic"
    headers.mkdir(exist_ok=True)
    for header in proto.glob("*.pb.h"):
        shutil.copyfile(header, headers / header.name)
    # Config uses the channel overload. The packet overloads in the same stock
    # file need nanopb only in tests that actually call the encoder/decoder.
    return [
        "-I" + str(output), "-I" + str(proto / "nanopb"),
        "-ffunction-sections", "-fdata-sections", dead_strip_flag(),
        str(app / "src/stock/position/PositionPrecision.cpp"),
    ]
