#!/usr/bin/env python3
"""Run the application role scratch, projection and journal callbacks under both native ABIs."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
APP=HERE.parents[1]
ROOT=APP.parent
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host

main=(APP/"src/main.cpp").read_text()
declarations=main[main.index("static mt::admin::RemoteDeviceTransaction remoteDeviceTransaction "):
                  main.index("static int recoverRemoteDeviceAtBoot()")]
def host_method(start,stop):
    return main[main.index("  int "+start+"("):main.index("  int "+stop+"(")]
methods=host_method("saveRemoteDevice","saveRemoteChannel")+host_method("saveRemoteLoRa","saveSecurityControls")
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"

with tempfile.TemporaryDirectory(prefix="zt-role-workspace-") as tmp:
    out=Path(tmp)
    (out/"journal.inc").write_text(declarations)
    (out/"setters.inc").write_text(methods)
    (out/"meshtastic").mkdir()
    for h in proto.glob("*.pb.h"):(out/"meshtastic"/h.name).symlink_to(h)
    for flags in ([],["-fshort-enums"]):
        flags=["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*flags,
               *(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),
               *["-I"+str(p) for p in (out,APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],
               *host.crypto_compile_flags()]
        objects=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/n for n in ("pb_common.c","pb_decode.c","pb_encode.c")]]:
            obj=out/(src.stem+".o");objects.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"cases.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",
                        vendor/"MtSignature.cpp",*objects,*host.crypto_link_flags(),"-o",out/"cases"],check=True)
        subprocess.run([out/"cases"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
