#!/usr/bin/env python3
"""Check the stock XModem state machine and encoded local PhoneAPI."""
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


src=source("src/xmodem.cpp")
donor="\n".join(method(src,signature) for signature in [
    "bool XModemAdapter::isValidFilename(", "unsigned short XModemAdapter::crc16_ccitt(",
    "int XModemAdapter::check(", "void XModemAdapter::sendControl(", "void XModemAdapter::handlePacket("])
proto=ROOT/"zephcore/bench/upstream-differential/source"
vendor=ROOT/"zephcore/adapters/bench/vendor"
mono=ROOT/"zephcore/lib/monocypher"
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="zt-phone-files-") as td:
    out=Path(td)
    (out/"stock.inc").write_text(donor)
    range_append=method(source("src/modules/RangeTestModule.cpp"),"bool RangeTestModuleRadio::appendFile(")
    free_guard=next(line.strip() for line in range_append.splitlines() if "FSCom.totalBytes() - FSCom.usedBytes()" in line)
    (out/"stock_range_space.inc").write_text(free_guard+" return false; }\nreturn true;\n")
    (out/"meshtastic").mkdir()
    for p in proto.glob("*.pb.h"):(out/"meshtastic"/p.name).symlink_to(p)
    for abi in [[],["-fshort-enums"]]:
        flags=[*(["-DEKEYREJECTED=129"] if sys.platform=="darwin" else []),"-DCONFIG_MT_PHONE_FILES=1","-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",*abi,
               *["-I"+str(v) for v in (out,APP/"tests/stock_admin_routing/fs",APP/"src",APP/"tests",APP/"src/pki",APP/"experimental/pki",vendor,mono,proto/"nanopb")],*host.crypto_compile_flags()]
        objs=[]
        for src in [mono/"monocypher.c",mono/"monocypher-ed25519.c",*[proto/"nanopb"/s for s in ("pb_common.c","pb_encode.c","pb_decode.c")]]:
            obj=out/(src.stem+".o");objs.append(obj)
            subprocess.run([*host.cc(),"-std=c11",*flags,"-c",src,"-o",obj],check=True)
        subprocess.run([*host.cxx(),"-std=c++17",*flags,HERE/"donor.cpp",HERE/"companion.cpp",HERE/"satellite.cpp",APP/"src/Protobuf.cpp",vendor/"MtCodec.cpp",vendor/"MtSignature.cpp",*objs,*host.crypto_link_flags(),"-o",out/"tests"],check=True)
        subprocess.run([out/"tests"],check=True,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})
    for index, mount in enumerate(("/mounted", "/public", "/zt-history/public")):
        directory=out/("mount-"+str(index));directory.mkdir()
        options=[f'-DCONFIG_MT_RANGE_TEST_LOG_DIR="{mount}"']
        if index:options.append("-DCONFIG_MT_PHONE_PUBLIC_TREE=1")
        if index==2:options.extend(["-DCONFIG_BOARD_XIAO_ESP32S3=1","-DCONFIG_FILE_SYSTEM_LITTLEFS=1"])
        subprocess.run([*host.cxx(),"-std=c++17","-O1","-fsanitize=address,undefined",
            "-DCONFIG_MT_PHONE_FILES=1","-DCONFIG_MT_RANGE_TEST_LOG=1","-DCONFIG_FILE_SYSTEM=1",*options,
            *["-I"+str(d) for d in (HERE/"fs",APP/"tests/stock_admin_routing/fs",APP/"src",out,proto/"nanopb")],
            HERE/"filesystem.cpp",APP/"src/modules/RangeTestLog.cpp","-o",out/"fs"],check=True)
        subprocess.run([out/"fs",directory],check=True)
