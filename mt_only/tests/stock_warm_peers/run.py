#!/usr/bin/env python3
"""Differentially test the exact pinned warm store against the paged implementation."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
APP=HERE.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stock",type=Path,required=True)
parser.add_argument("--littlefs",type=Path,required=True)
args=parser.parse_args()
pin="8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
def source(path):
    return subprocess.check_output(["git","-C",args.stock,"show",pin+":"+path],text=True)
def body(source,begin,end):
    return source[source.index(begin):source.index(end,source.index(begin))]
header=source("src/mesh/WarmNodeStore.h")
cpp=source("src/mesh/WarmNodeStore.cpp")
db=source("src/mesh/NodeDB.cpp")
oracle=body(header,"struct WarmNodeEntry","// Gated on NRF") + body(header,"class WarmNodeStore","    // Persistence hooks")
oracle+="    void persistEntry(const WarmNodeEntry&){}\n    void persistRemove(NodeNum,int){}\n    void persistClear(){}\n};\n"
oracle+=body(cpp,"static inline bool keyIsSet","bool WarmNodeStore::saveIfDirty()")
oracle+=body(db,"static uint8_t warmProtectedCategory","// The warm tier packs")
assert "warmStore.absorb(evicted.num" in db and "if (warmStore.take(n, warm))" in db
constants=source("src/mesh/mesh-pb-constants.h")
assert "#define WARM_NODE_COUNT 2000" in constants and "#define WARM_NODE_COUNT 100" in constants
import sys
sys.path.insert(0,str(APP.parent))
import host_test_toolchain as host
import hashlib
hashes={"lfs.c":"e36dfc19a2e8516024d764c9713c85a2b26a80cf0f4d9cd862c4104771e170e8","lfs.h":"b1befd7288d08815accc8f9af744c55686c0b9e3ac0061c32ceee38a1b3eb96d","lfs_util.c":"f2fbde533670560434bd9f5a547174cc7c5a4670a02c47b4bd85180dced8b2ec","lfs_util.h":"548d46aa524dc7449e16739286c1a422a52f9de727ff0be0c2ffc5593f5ca981"}
with tempfile.TemporaryDirectory(prefix="zt-warm-peers-") as tmp:
    out=Path(tmp)
    (out/"oracle.inc").write_text(oracle)
    root=APP.parent
    proto=root/"zephcore/bench/upstream-differential/source"
    mono=root/"zephcore/lib/monocypher";vendor=root/"zephcore/adapters/bench/vendor"
    (out/"meshtastic").mkdir()
    for h in proto.glob("*.pb.h"):(out/"meshtastic"/h.name).symlink_to(h)
    for name,digest in hashes.items():
        data=(args.littlefs/name).read_bytes()
        if hashlib.sha256(data).hexdigest()!=digest:raise ValueError(name+" differs from the pinned SDK")
        (out/name).write_bytes(data)
    for abi in ([],["-fshort-enums"]):
        flags=["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer","-DCONFIG_MT_WARM_PEERS=1",
               "-DLFS_NO_DEBUG","-DLFS_NO_WARN","-DLFS_NO_ERROR","-D__maybe_unused=__attribute__((unused))",
               "-I"+str(out),"-I"+str(HERE/"mocks"),"-I"+str(APP/"src"),*abi]
        for name in ("lfs","lfs_util"):
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",out/(name+".c"),"-o",out/(name+".o")],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"cases.cpp",out/"lfs.o",out/"lfs_util.o","-o",out/"test"],check=True)
        subprocess.run([out/"test"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
        flags+=[*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-DCONFIG_MT_PEER_DIRECTORY=1",
                *["-I"+str(v) for v in (APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        objects=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/name for name in ("pb_common.c","pb_decode.c","pb_encode.c")]]:
            obj=out/(src.stem+".o");objects.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"radio.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objects,*host.crypto_link_flags(),"-o",out/"radio"],check=True)
        subprocess.run([out/"radio"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
