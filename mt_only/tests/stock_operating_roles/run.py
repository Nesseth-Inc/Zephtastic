#!/usr/bin/env python3
"""Compare production operating role behavior with pinned stock defaults."""
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
def method(unit,signature):
    i=unit.index(signature)
    return unit[i:unit.index("\n}",i)+2]+'\n'


src=source("src/mesh/NodeDB.cpp")
atak=method(source("src/modules/AtakPluginModule.cpp"),"ProcessMessage AtakPluginModule::handleReceived(")
assert [line.strip() for line in atak.splitlines()[2:-1]]==["(void)mp; // Passthrough - no processing needed, apps handle compression/decompression","return ProcessMessage::CONTINUE;"]
donor=""
for role in ("SENSOR","TRACKER","LOST_AND_FOUND","TAK","CLIENT_HIDDEN"):
    start=src.index(f"if (role == meshtastic_Config_DeviceConfig_Role_{role})", src.index("void NodeDB::installRoleDefaults"))
    donor+=src[start:src.index("\n    }",start)+6]+"\n"
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="zt-operating-roles-") as td:
    out=Path(td)
    (out/"donor.inc").write_text(donor)
    main=(APP/"src/main.cpp").read_text()
    start=main.index("    if(mt::modules::OperatingRole::handles(c.role())")
    stop=main.index("\n    }",start)+6
    (out/"application-role-save.inc").write_text(main[start:stop].replace("mt::notification::available()","true"))
    (out/"meshtastic").mkdir()
    for p in proto.glob("*.pb.h"):(out/"meshtastic"/p.name).symlink_to(p)
    for abi in [[],["-fshort-enums"]]:
        flags=[*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *["-I"+str(v) for v in (out,HERE/"mocks",APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        objs=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objs.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"roles.cpp",HERE/"admin.cpp",APP/"src/power/PowerPolicy.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objs,*host.crypto_link_flags(),"-o",out/"tests"],check=True)
        subprocess.run([out/"tests"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
