#!/usr/bin/env python3
"""Exercise the bounded private peer directory with two real PhoneAPI handshake modes."""
from pathlib import Path
import argparse, os, subprocess, sys, tempfile
APP=Path(__file__).resolve().parents[2]
ROOT=APP.parent
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host
P=argparse.ArgumentParser(description=__doc__)
P.add_argument("--stock",required=True,type=Path)
args=P.parse_args()
pin="8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
def source(path):
    return subprocess.check_output(["git","-C",args.stock,"show",f"{pin}:{path}"],text=True)
phone=source("src/mesh/PhoneAPI.cpp")
assert "nodeDB->getNumMeshNodes()" in phone and "STATE_SEND_OTHER_NODEINFOS" in phone
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="zt-peer-directory-") as td:
    out=Path(td)
    (out/"meshtastic").mkdir()
    for p in proto.glob("*.pb.h"):(out/"meshtastic"/p.name).symlink_to(p)
    for abi in [[],["-fshort-enums"]]:
        flags=[*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-DCONFIG_MT_PEER_DIRECTORY=1","-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *["-I"+str(v) for v in (out,APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        objs=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objs.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        for extra in ([],["-DCONFIG_MT_XIAO_LARGE_NODEDB=1"]):
            subprocess.run([*host.cxx(),"-std=c++17",*flags,*extra,HERE/"cases.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objs,*host.crypto_link_flags(),"-o",out/"tests"],check=True)
            subprocess.run([out/"tests"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
