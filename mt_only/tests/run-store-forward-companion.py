#!/usr/bin/env python3
"""Run real Service/Companion StoreForward phone replay integration tests."""
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host

APP=ROOT/"mt_only"
PROTO=ROOT/"zephcore/bench/upstream-differential/source"
NANOPB=PROTO/"nanopb"
VENDOR=ROOT/"zephcore/adapters/bench/vendor"
MONO=ROOT/"zephcore/lib/monocypher"
TEST=APP/"tests/store_forward_companion_tests.cpp"

def run(argv,env):
    print("$"," ".join(map(str,argv)),flush=True)
    result=subprocess.run(list(map(str,argv)),env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if result.stdout: print(result.stdout,end="",flush=True)
    if result.returncode: raise SystemExit(result.returncode)

def main():
    if not all(path.exists() for path in (TEST,PROTO/"mesh.pb.h",NANOPB,VENDOR,MONO)):
        raise SystemExit("required test, pinned protobuf, codec, or crypto source missing")
    cc=shlex.split(os.environ.get("CC","cc"));cxx=shlex.split(os.environ.get("CXX","c++"))
    env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",UBSAN_OPTIONS="halt_on_error=1")
    with tempfile.TemporaryDirectory(prefix="m137-store-forward-companion-") as tmp:
        build=Path(tmp);generated=build/"meshtastic";generated.mkdir()
        for header in PROTO.glob("*.pb.h"): shutil.copyfile(header,generated/header.name)
        common=["-fsanitize=address,undefined","-fno-omit-frame-pointer","-g","-O1","-Wall","-Wextra","-Werror",
                "-DCONFIG_MT_STORE_FORWARD_PSRAM=1","-DMT_STORE_FORWARD_MEMORY_NATIVE_TEST=1",
                "-I"+str(build),"-I"+str(PROTO),"-I"+str(NANOPB),"-I"+str(APP/"src"),
                "-I"+str(APP/"tests"),"-I"+str(VENDOR),"-I"+str(MONO),*host.crypto_compile_flags()]
        c_sources=[NANOPB/"pb_common.c",NANOPB/"pb_encode.c",NANOPB/"pb_decode.c",
                   MONO/"monocypher.c",MONO/"monocypher-ed25519.c"]
        cpp_sources=[TEST,APP/"src/Protobuf.cpp",APP/"src/modules/StoreForwardMemory.cpp",
                     APP/"src/modules/StoreForwardWire.cpp",VENDOR/"MtCodec.cpp",VENDOR/"MtSignature.cpp"]
        for label,enum_flags in (("default-enums",[]),("short-enums",["-fshort-enums"])):
            objects=[]
            for source in c_sources:
                obj=build/(label+"-"+source.stem+".o")
                run([*cc,"-std=c11",*enum_flags,*common,"-c",source,"-o",obj],env);objects.append(obj)
            executable=build/("store-forward-companion-"+label)
            run([*cxx,"-std=c++17",*enum_flags,*common,*cpp_sources,*objects,*host.crypto_link_flags(),"-o",executable],env)
            run([executable],env)

if __name__=="__main__": main()
