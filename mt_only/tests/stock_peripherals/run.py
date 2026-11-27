#!/usr/bin/env python3
"""Compare production peripheral policies with executable handlers from the pinned firmware."""
import argparse
import os
import pathlib
import shlex
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
APP = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--stock", required=True, type=pathlib.Path)
parser.add_argument("--release", choices=("2.8.1", "2.8.2"), default="2.8.1")
args = parser.parse_args()
PIN = "d5f16191ca28b01b5024f590f04a67f25d66f1b1" if args.release == "2.8.2" else "8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c"


def source(name):
    return subprocess.check_output(["git", "-C", args.stock, "show", f"{PIN}:{name}"], text=True)


def method(text, start):
    i = text.index(start)
    return text[i:text.index("\n}", i) + 2] + "\n"


serial = source("src/modules/SerialModule.cpp")
canned = source("src/modules/CannedMessageModule.cpp")
external = source("src/modules/ExternalNotificationModule.cpp")
detection = source("src/modules/DetectionSensorModule.cpp")
ambient = source("src/AmbientLightingThread.h")
methods = [
    method(serial, "ProcessMessage SerialModuleRadio::handleReceived("),
    method(canned, "int CannedMessageModule::splitConfiguredMessages("),
    method(canned, "int CannedMessageModule::getNextIndex("),
    method(canned, "int CannedMessageModule::getPrevIndex("),
    method(canned, "void CannedMessageModule::handleSetCannedMessageModuleMessages("),
    method(external, "ProcessMessage ExternalNotificationModule::handleReceived("),
    method(external, "void ExternalNotificationModule::armNagCycle("),
    method(external, "int ExternalNotificationModule::handleInputEvent("),
    method(external, "int32_t ExternalNotificationModule::runOnce("),
    method(external, "void ExternalNotificationModule::stopNow("),
    detection[detection.index("typedef enum {"):detection.index("// The configured trigger type")],
    method(detection, "int32_t DetectionSensorModule::runOnce("),
    method(detection, "bool DetectionSensorModule::hasDetectionEvent()"),
    method(detection, "void DetectionSensorModule::sendDetectionMessage()"),
    method(detection, "void DetectionSensorModule::sendCurrentStateMessage("),
]

# Compile the stock send path through the mesh handoff. Its remaining code
# records UI messages/favorites; ZT's existing UI owner handles local history.
send = method(canned, "void CannedMessageModule::sendText(")
handoff = "service->sendToMesh(p, RX_SRC_LOCAL, true);"
methods.append(send[:send.index(handoff)+len(handoff)]+"\n}\n")

with tempfile.TemporaryDirectory(prefix="zt-pinned-peripherals-") as temp:
    tmp = pathlib.Path(temp)
    (tmp / "upstream.inc").write_text("".join(methods))
    nmea = source("src/gps/NMEAWPL.cpp")
    geo = method(source("src/gps/GeoCoord.cpp"), "void GeoCoord::latLongToDMS(")
    start = serial.index("        float velAvg =")
    end = serial.index('        LOG_INFO("WS8X Transmit', start)
    (tmp / "donor_ws85.inc").write_text(serial[start:end])
    (tmp / "donor_nmea.inc").write_text(geo + "\n".join(line for line in nmea.splitlines() if not line.startswith("#include")))
    (tmp / "AmbientLightingThread.h").write_text(ambient)
    for header in ["Observer.h", "configuration.h", "detect/ScanI2C.h", "sleep.h"]:
        p = tmp / header
        p.parent.mkdir(exist_ok=True)
        p.touch()
    proto = APP.parent / "zephcore/bench/upstream-differential/source"
    (tmp / "meshtastic").mkdir()
    for header in proto.glob("*.pb.h"):
        (tmp / "meshtastic" / header.name).symlink_to(header)
    for name, extra in [("default", []), ("short-enums", ["-fshort-enums"])]:
        binary = tmp / name
        objects = []
        for unit in ["pb_common", "pb_decode", "pb_encode"]:
            obj = tmp / f"{name}-{unit}.o"
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O1",
                            "-fsanitize=address,undefined", *extra, "-c", str(proto / "nanopb" / (unit + ".c")),
                            "-o", str(obj)], check=True)
            objects.append(str(obj))
        cmd = [*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
               "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *extra,
               "-I", str(tmp), "-I", str(APP.parent / "zephcore/adapters/bench/vendor"),
               "-I", str(APP.parent / "zephcore/lib/monocypher"), "-I", str(proto / "nanopb"), "-I", str(APP / "src"),
               str(HERE / "compare.cpp"), str(HERE / "nmea_cases.cpp"),
               str(HERE / "weather_cases.cpp"), str(APP / "src/serial/Ws85Engine.cpp"),
               str(HERE / "notification_cases.cpp"), str(APP / "src/notifications/ExtendedNotificationController.cpp"), str(APP / "src/serial/PositionOutput.cpp"),
               str(APP / "src/stock/peripherals/NMEAWPL.cpp"), str(APP / "src/serial/SerialEngine.cpp"), *objects, "-o", str(binary)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
