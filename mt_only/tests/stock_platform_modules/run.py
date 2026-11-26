#!/usr/bin/env python3
"""Compare the pinned report and watchdog bodies, USB bench commands and host bridge with the production companion."""
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

device=source("src/modules/Telemetry/DeviceTelemetry.cpp")
power=source("src/modules/Telemetry/PowerTelemetry.cpp")
environment=source("src/modules/Telemetry/EnvironmentTelemetry.cpp")
wd=source("src/watchdog/watchdogThread.cpp")
atak=source("src/modules/AtakPluginModule.cpp")
donor=method(device,"meshtastic_Telemetry DeviceTelemetryModule::getDeviceTelemetry()")
donor+=method(power,"bool PowerTelemetryModule::getPowerTelemetry(")
donor+=method(environment,"bool EnvironmentTelemetryModule::getEnvironmentTelemetry(")
donor+=method(wd,"void WatchdogThread::feedDog(")+method(wd,"bool WatchdogThread::setup(")
donor+=method(atak,"ProcessMessage AtakPluginModule::handleReceived(")
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="zt-platform-") as td:
    out=Path(td)
    (out/"stock.inc").write_text(donor)
    (out/"meshtastic").mkdir()
    for p in proto.glob("*.pb.h"):(out/"meshtastic"/p.name).symlink_to(p)
    for abi in [[],["-fshort-enums"],["-DPB_BUFFER_ONLY","-DPB_NO_ERRMSG"],["-DPB_BUFFER_ONLY","-DPB_NO_ERRMSG","-fshort-enums"]]:
        flags=["-DCONFIG_MT_HOST_METRICS_BRIDGE=1","-DCONFIG_MT_POWER_STRESS=1",*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *["-I"+str(v) for v in (out,APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        objs=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objs.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"cases.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objs,*host.crypto_link_flags(),"-o",out/"tests"],check=True)
        subprocess.run([out/"tests"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
