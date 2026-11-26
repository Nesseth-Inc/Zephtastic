#!/usr/bin/env python3
"""Check four optional stock handlers against their pinned implementations and actual radio dispatch."""
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

bot=source("src/modules/ReplyBotModule.cpp")
drop=source("src/modules/DropzoneModule.cpp")
conversion_header=source("src/modules/Telemetry/UnitConversions.h")
donor=conversion_header[conversion_header.index("class UnitConversions"):]+"\n"
donor+=source("src/modules/Telemetry/UnitConversions.cpp").split('#include "UnitConversions.h"',1)[1]+"\n"
donor+=bot[bot.index("struct ReplyBotCooldownEntry"):bot.index("// Constructor")]
for signature in ["ProcessMessage ReplyBotModule::handleReceived","bool ReplyBotModule::isCommand","void ReplyBotModule::sendDm"]:
    donor+=method(bot,signature)
donor+=method(source("src/mesh/NodeDB.cpp"),"int8_t getHopsAway(")
for signature in ["int32_t DropzoneModule::runOnce","ProcessMessage DropzoneModule::handleReceived","meshtastic_MeshPacket *DropzoneModule::sendConditions"]:
    donor+=method(drop,signature)
donor+=method(source("src/modules/ReplyModule.cpp"),"meshtastic_MeshPacket *ReplyModule::allocReply")
donor+=method(source("src/modules/GenericThreadModule.cpp"),"int32_t GenericThreadModule::runOnce")
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="zt-optional-") as td:
    out=Path(td)
    (out/"stock.inc").write_text(donor)
    (out/"meshtastic").mkdir()
    for p in proto.glob("*.pb.h"):(out/"meshtastic"/p.name).symlink_to(p)
    for abi in [[],["-fshort-enums"]]:
        flags=[*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-DCONFIG_MT_REPLY_BOT=1","-DCONFIG_MT_REPLY_EXAMPLE=1","-DCONFIG_MT_DROPZONE=1","-DCONFIG_MT_GENERIC_THREAD=1","-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *["-I"+str(v) for v in (out,APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        objs=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objs.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"cases.cpp",HERE/"endpoint.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objs,*host.crypto_link_flags(),"-o",out/"tests"],check=True)
        subprocess.run([out/"tests"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
        for mapped in (0,1):
            board=out/("board-"+str(mapped))
            subprocess.run([*host.cxx(),"-std=c++17",*flags,f"-DMT_MOCK_HAS_ADC={mapped}","-I"+str(HERE/"mocks"),
                            "-I"+str(APP/"tests/hardware_metrics/mocks"),HERE/"board.cpp","-o",board],check=True)
            subprocess.run([board],check=True)
