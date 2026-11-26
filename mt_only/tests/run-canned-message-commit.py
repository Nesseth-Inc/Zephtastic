#!/usr/bin/env python3
"""Run the isolated canned-message storage composition tests under sanitizers."""
import hashlib
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
APP=ROOT/"mt_only"
VENDOR=ROOT/"zephcore/adapters/bench/vendor"
PROTO=ROOT/"zephcore/bench/upstream-differential/source"
NANOPB=PROTO/"nanopb"
MONO=ROOT/"zephcore/lib/monocypher"
SOURCES=(APP/"tests/canned_message_commit_tests.cpp",APP/"src/modules/CannedMessageCommit.h",
         APP/"src/modules/CannedMessageJournal.h",APP/"src/modules/CannedMessageSettings.h",
         APP/"src/SettingsJournal.h",APP/"src/Config.h",APP/"src/Service.h",
         APP/"tests/known_channel_fixture.h",VENDOR/"MtNodeInfo.h",
         VENDOR/"MtCodec.cpp",VENDOR/"MtSignature.cpp",MONO/"monocypher.c",
         MONO/"monocypher-ed25519.c")

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(command,env):
    print("$"," ".join(map(str,command)),flush=True)
    result=subprocess.run(command,env=env,text=True,capture_output=True)
    if result.stdout:print(result.stdout,end="",flush=True)
    if result.stderr:print(result.stderr,end="",flush=True)
    if result.returncode:raise SystemExit(result.returncode)

def main():
    before={str(p):digest(p) for p in SOURCES}
    env=dict(os.environ);env["ASAN_OPTIONS"]="detect_leaks=0:halt_on_error=1";env["UBSAN_OPTIONS"]="halt_on_error=1"
    cxx=shlex.split(env.get("CXX","c++"))
    with tempfile.TemporaryDirectory(prefix="mt-canned-commit-") as temporary:
        out=Path(temporary);generated=out/"meshtastic";generated.mkdir()
        for header in PROTO.glob("*.pb.h"):shutil.copyfile(header,generated/header.name)
        common=["-fsanitize=address,undefined","-fno-omit-frame-pointer","-g","-O1",
                "-I"+str(out),"-I"+str(NANOPB),"-I"+str(APP/"src"),
                "-I"+str(APP/"tests"),"-I"+str(VENDOR),"-I"+str(MONO)]
        for label,extra in (("default-enum",[]),("short-enum",["-fshort-enums"])):
            objects=[]
            for source in (NANOPB/"pb_common.c",NANOPB/"pb_encode.c",NANOPB/"pb_decode.c",
                           MONO/"monocypher.c",MONO/"monocypher-ed25519.c"):
                obj=out/(label+"-"+source.stem+".o")
                run([*shlex.split(env.get("CC","cc")),"-std=c11",*common,*extra,
                     "-Wall","-Wextra","-c",str(source),"-o",str(obj)],env)
                objects.append(str(obj))
            exe=out/("canned-commit-"+label)
            run([*cxx,"-std=c++17","-Wall","-Wextra","-Werror",*common,*extra,
                 str(SOURCES[0]),str(APP/"src/Protobuf.cpp"),str(VENDOR/"MtCodec.cpp"),
                 str(VENDOR/"MtSignature.cpp"),*objects,"-o",str(exe)],env)
            run([str(exe)],env)
    after={str(p):digest(p) for p in SOURCES}
    assert before==after,"source changed during native commit tests"
    print("PASS canned-message commit default/short-enum ASan/UBSan variants; source pins unchanged")

if __name__=="__main__":main()
