#!/usr/bin/env python3
"""Compare LoRa sender filtering with the pinned RF and MQTT source predicates."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
APP=HERE.parents[1]
ROOT=APP.parent
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stock",type=Path,required=True)
args=parser.parse_args()
pin="8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"

def source(path):
    return subprocess.check_output(["git","-C",args.stock,"show",pin+":"+path],text=True)

def method(text,signature):
    begin=text.index(signature)
    return text[begin:text.index("\n}",begin)+2]

constants=source("src/mesh/mesh-pb-constants.h")
membership=method(source("src/mesh/mesh-pb-constants.cpp"),"bool is_in_helper(")
macro=next(line for line in constants.splitlines() if line.startswith("#define is_in_repeated("))
router=method(source("src/mesh/Router.cpp"),"void Router::perhapsHandleReceived(")
mqtt=method(source("src/mqtt/MQTT.cpp"),"inline bool shouldDropMqttDownlink(")
def predicate(text,term):
    line=next(line.strip() for line in text.splitlines() if term in line)
    assert line.startswith("if (") and line.endswith(") {")
    return line[4:-3]

rf=predicate(router,"is_in_repeated(")
broker=predicate(mqtt,"is_in_repeated(")
assert router.index(rf)<router.index("nodeDB->getMeshNode(")<router.index("passesRoutingAuthGate(")
assert mqtt.index(broker)<mqtt.index("nodeDB->getMeshNode(")
mqtt_receive=method(source("src/mqtt/MQTT.cpp"),"inline void onReceiveProto(")
assert mqtt_receive.index("if (isFromUs(e.packet))")<mqtt_receive.index("if (shouldDropMqttDownlink(*p))")<mqtt_receive.index("if (p->which_payload_variant")
admin=source("src/modules/AdminModule.cpp")
assert "config.lora = validatedLora;" in admin
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
with tempfile.TemporaryDirectory(prefix="zt-stock-ignore-") as tmp:
    out=Path(tmp)
    (out/"oracle.inc").write_text(macro+"\nstatic "+membership+"\n"+
        "bool radio(const meshtastic_MeshPacket *p)const{return "+rf+";}\n"+
        "bool mqtt(const meshtastic_MeshPacket &packet)const{return "+broker+";}\n")
    (out/"meshtastic").mkdir()
    for h in proto.glob("*.pb.h"):(out/"meshtastic"/h.name).symlink_to(h)
    for abi in ([],["-fshort-enums"]):
        flags=["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),
               *["-I"+str(p) for p in (out,APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],
               *host.crypto_compile_flags()]
        objects=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objects.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"cases.cpp",APP/"src/Protobuf.cpp",
                        vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objects,*host.crypto_link_flags(),"-o",out/"cases"],check=True)
        subprocess.run([out/"cases"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
