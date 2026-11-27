#!/usr/bin/env python3
"""Compare Waypoint, Geofence and StatusLED with pinned handlers and the production companion receiver."""
from pathlib import Path
import argparse, os, subprocess, sys, tempfile
APP=Path(__file__).resolve().parents[2]
ROOT=APP.parent
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host
P=argparse.ArgumentParser(description=__doc__)
P.add_argument("--stock",required=True,type=Path)
P.add_argument("--release", choices=("2.8.1","2.8.2"), default="2.8.1")
args=P.parse_args()
pin="d5f16191ca28b01b5024f590f04a67f25d66f1b1" if args.release=="2.8.2" else "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"
def source(path):
    return subprocess.check_output(["git","-C",args.stock,"show",f"{pin}:{path}"],text=True)
def method(unit,signature):
    i=unit.index(signature)
    return unit[i:unit.index("\n}",i)+2]+'\n'

way=source("src/WaypointStore.cpp")
geo=source("src/modules/GeofenceModule.cpp")
led=source("src/modules/StatusLEDModule.cpp")
donor=method(source("src/meshUtils.h"),"static inline bool waypointIsActive(")
for signature in ["bool WaypointStore::isExpired(const meshtastic_Waypoint", "bool WaypointStore::isExpired(const StoredWaypoint",
                  "uint8_t WaypointStore::notificationPreferencesFromWaypoint", "uint8_t WaypointStore::mergeNotificationPreferences",
                  "void WaypointStore::clearWireNotificationPreferences","const StoredWaypoint *WaypointStore::findWaypoint",
                  "bool WaypointStore::removeWaypointById","bool WaypointStore::removeWaypoint(","void WaypointStore::addStoredWaypoint",
                  "bool WaypointStore::addFromPacket","bool WaypointStore::purgeExpired"]:
    donor+=method(way,signature)
geo_coord=source("src/gps/GeoCoord.cpp")
donor+=method(geo_coord,"static double cosLatitudeApprox(")+method(geo_coord,"float GeoCoord::latLongToMeter(")
for signature in ["bool GeofenceModule::insideRadius","bool GeofenceModule::insideBox","bool GeofenceModule::insideAny",
                  "bool GeofenceModule::inside(","bool GeofenceModule::hasGeofence","GeofenceModule::Crossing GeofenceModule::classify",
                  "bool GeofenceModule::shouldTrack"]:
    donor+=method(geo,signature)
donor+=method(led,"int StatusLEDModule::handleLoRaRx")
start=led.index("    if (LORA_LED_state",led.index("// End the LoRa-RX flash"))
donor+="void StatusLEDModule::poll(){int my_interval=1000;\n"+led[start:led.index("#endif",start)]+"}\n"
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="zt-waypoint-") as td:
    out=Path(td)
    (out/"stock.inc").write_text(donor)
    (out/"meshtastic").mkdir()
    for p in proto.glob("*.pb.h"):(out/"meshtastic"/p.name).symlink_to(p)
    for abi in [[],["-fshort-enums"]]:
        flags=[*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *["-I"+str(v) for v in (out,APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        objs=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objs.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"cases.cpp",HERE/"endpoint.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objs,*host.crypto_link_flags(),"-o",out/"tests"],check=True)
        subprocess.run([out/"tests"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
