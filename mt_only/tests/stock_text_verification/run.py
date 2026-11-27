#!/usr/bin/env python3
"""Compare Zephyr handlers against pinned donor code and a two-node verification exchange."""
import argparse
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile

PIN = "d5f16191ca28b01b5024f590f04a67f25d66f1b1"
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PROTO = ROOT / "zephcore/bench/upstream-differential/source"
NANOPB = PROTO / "nanopb"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    args = parser.parse_args()

    def source(path):
        return subprocess.check_output(["git", "-C", args.stock, "show", f"{PIN}:src/{path}"], text=True)

    def method(text, signature):
        start = text.index(signature)
        return text[start:text.index("\n}", start)+2]

    text = source("modules/TextMessageModule.cpp")
    kv = source("modules/KeyVerificationModule.cpp")
    verify = method(kv, "void KeyVerificationModule::processSecurityNumber(")
    # Keep the donor SHA updates, validation, and proof construction. Its NodeDB
    # key lookup and Arduino packet send are supplied by this harness.
    proof = verify[verify.index("    // calculate hash1"):verify.index("    meshtastic_MeshPacket *p =")]
    proof += "    capture = KeyVerification;\n}\n"
    want = method(source("mesh/MeshService.h").replace("\n    }", "\n}"), "static bool isTextPayload(")
    wake = method(source("graphics/Screen.cpp"), "bool shouldWakeOnReceivedMessage()")
    alert = method(source("mesh/MeshService.cpp"), "bool MeshService::isAlertPayload(")
    compiled = "\n".join([alert,wake,method(text,"ProcessMessage TextMessageModule::handleReceived("),
                          method(text,"bool TextMessageModule::wantPacket("),
                          method(kv,"void KeyVerificationModule::generateVerificationCode(")])
    # Private Linux runners may extract OpenSSL packages into a job-local
    # prefix already on CPLUS_INCLUDE_PATH/LIBRARY_PATH.
    prefix = os.environ.get("MT_TEST_OPENSSL_PREFIX")
    if prefix is None and platform.system() == "Darwin":
        prefix = "/opt/homebrew/opt/openssl@3"
    crypto_includes = [Path(prefix) / "include"] if prefix else []
    crypto_libs = ["-L", Path(prefix) / "lib"] if prefix else []
    with tempfile.TemporaryDirectory(prefix="zt-text-verification-") as tmp:
        out = Path(tmp)
        (out/"meshtastic").mkdir()
        for header in PROTO.glob("*.pb.h"):
            (out/"meshtastic"/header.name).symlink_to(header)
        (out/"want.inc").write_text(want)
        (out/"oracle.inc").write_text(compiled)
        (out/"proof.inc").write_text(proof)
        for short in (False,True):
            flags = ["-fsanitize=address,undefined","-fno-omit-frame-pointer","-O1"]
            if short: flags.append("-fshort-enums")
            objects = []
            for n in ("pb_common","pb_encode","pb_decode"):
                obj = out/(n+".o")
                subprocess.run([*shlex.split(os.environ.get("CC","cc")),*flags,"-c",NANOPB/(n+".c"),"-o",obj],check=True)
                objects.append(obj)
            inc = [v for path in (out,NANOPB,ROOT/"mt_only/src",*crypto_includes) for v in ("-I",path)]
            binary=out/"run-comparison"
            subprocess.run([*shlex.split(os.environ.get("CXX","c++")),"-std=c++17",*flags,*inc,
                            *(["-DEKEYREJECTED=129"] if platform.system()=="Darwin" else []),
                            ROOT/"mt_only/src/Protobuf.cpp",ROOT/"mt_only/src/pki/verification/KeyVerification.cpp",
                            HERE/"cases.cpp",*objects,*crypto_libs,"-lcrypto","-o",binary],check=True)
            subprocess.run([binary],check=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=0:halt_on_error=1",
                                                   "UBSAN_OPTIONS":"halt_on_error=1:print_stacktrace=1"})

if __name__=="__main__":
    main()
