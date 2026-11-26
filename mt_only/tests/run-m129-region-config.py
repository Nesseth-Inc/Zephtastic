#!/usr/bin/env python3
"""Run focused M129 Config migration and Companion region-wire tests."""
import hashlib
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
APP=ROOT/"mt_only"
PROTO=ROOT/"zephcore/bench/upstream-differential/source"
NANOPB=PROTO/"nanopb"
VENDOR=ROOT/"zephcore/adapters/bench/vendor"
MONO=ROOT/"zephcore/lib/monocypher"


def run(command,env=None):
    print("$"," ".join(map(str,command)),flush=True)
    subprocess.run(list(map(str,command)),check=True,env=env)


def main():
    cc=shlex.split(os.environ.get("CC","clang"))
    cxx=shlex.split(os.environ.get("CXX","clang++"))
    with tempfile.TemporaryDirectory(prefix="mt-m129-region-config-") as temp:
        build=Path(temp);generated=build/"meshtastic";generated.mkdir()
        # Compile the actual pinned get-slot and apply-modem expressions on the host.
        source=(PROTO/"RadioInterface.cpp").read_bytes()
        assert hashlib.sha256(source).hexdigest()=="0de8434b14c3c1deb383b4510b96a9d0d750a797e9140f9cfb08147e4a749506"
        unit=source.decode()
        start=unit.index("uint32_t hash(const char *str)")
        (build/"my919_stock_hash.inc").write_text(unit[start:unit.index("\n}",start)+2]+"\n")
        body=unit[unit.index("void RadioInterface::applyModemConfig()"):]
        slot=body[body.index("    float freqSlotWidth ="):body.index("    if (newRegion->wideLora)")]
        begin=unit.index("        uses_default_frequency_slot =")
        selected=unit[begin:unit.index("        // check if user setting",begin)]
        slot=slot.replace("    // override if we have a verbatim frequency",selected+"\n    // override if we have a verbatim frequency")
        (build/"my919_stock_slot.inc").write_text(slot)
        for header in PROTO.glob("*.pb.h"):
            shutil.copyfile(header,generated/header.name)
        common=["-fsanitize=address,undefined","-fno-omit-frame-pointer","-g","-O1",
                "-I"+str(APP/"src"),"-I"+str(APP/"tests"),"-I"+str(VENDOR),
                "-I"+str(MONO),"-I"+str(build),"-I"+str(NANOPB)]
        host_link=[]
        if os.name=="posix" and __import__("sys").platform.startswith("linux"):
            common.append("-DMT_HOST_OPENSSL=1");host_link=["-lcrypto"]
        env={**os.environ,"ASAN_OPTIONS":"detect_leaks=0:halt_on_error=1",
             "UBSAN_OPTIONS":"halt_on_error=1"}
        for label,enum_flags in (("default-enums",[]),("short-enums",["-fshort-enums"])):
            objects=[]
            for source in (MONO/"monocypher.c",MONO/"monocypher-ed25519.c",
                           NANOPB/"pb_common.c",NANOPB/"pb_encode.c",NANOPB/"pb_decode.c"):
                obj=build/(label+"-"+source.stem+".o")
                run([*cc,"-std=c11",*common,*enum_flags,"-Wall","-Wextra","-c",source,"-o",obj])
                objects.append(obj)
            exe=build/("m129-region-"+label)
            run([*cxx,"-std=c++17","-Wall","-Wextra","-Werror",*common,*enum_flags,
                 APP/"tests/m129_region_config_tests.cpp",APP/"src/Protobuf.cpp",
                 VENDOR/"MtCodec.cpp",VENDOR/"MtSignature.cpp",*objects,*host_link,"-o",exe])
            run([exe],env)
            rx=build/("xiao-my919-"+label)
            run([*cxx,"-std=c++17","-Wall","-Wextra","-Werror",*common,*enum_flags,
                 "-DCONFIG_BOARD_XIAO_ESP32S3=1","-I"+str(APP/"tests/stock_admin_routing/fs"),
                 APP/"tests/my919_receive.cpp",APP/"src/Protobuf.cpp",APP/"src/modules/BeaconTargetRuntime.cpp",
                 APP/"src/modules/BeaconExcursion.cpp",VENDOR/"MtCodec.cpp",VENDOR/"MtSignature.cpp",*objects,*host_link,"-o",rx])
            run([rx],env)
            catalogue_exe=build/("m129-region-catalogue-"+label)
            run([*cxx,"-std=c++17","-Wall","-Wextra","-Werror",*common,*enum_flags,
                 APP/"tests/m129_region_catalogue_companion_tests.cpp",APP/"src/Protobuf.cpp",
                 VENDOR/"MtCodec.cpp",VENDOR/"MtSignature.cpp",*objects,*host_link,"-o",catalogue_exe])
            run([catalogue_exe],env)


if __name__=="__main__":
    main()
