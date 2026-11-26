#!/usr/bin/env python3
"""Focused default/short-enum ASan+UBSan GPIO runtime fixture."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
from pathlib import Path

APP=Path(__file__).resolve().parents[1]
ROOT=APP.parent
PROTO=ROOT/"zephcore/bench/upstream-differential/source"
NANOPB=PROTO/"nanopb"
TEST=APP/"tests/m138_gpio_runtime_tests.cpp"
SOURCES=[APP/"src/modules/GpioModuleRuntime.h",APP/"src/modules/GpioModuleBackend.h",
 APP/"src/modules/GpioModuleSettings.h",APP/"src/modules/RemoteHardwarePolicy.h",
 APP/"src/modules/DetectionSensorPolicy.h",APP/"tests/m138_gpio_runtime_tests.cpp",
 APP/"src/Protobuf.cpp",PROTO/"module_config.pb.h",PROTO/"remote_hardware.pb.h",
 NANOPB/"pb.h",NANOPB/"pb_common.h",NANOPB/"pb_encode.h",NANOPB/"pb_decode.h",
 NANOPB/"pb_common.c",NANOPB/"pb_encode.c",NANOPB/"pb_decode.c"]
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def run(argv,env,log):
 log.write("$ "+shlex.join(map(str,argv))+"\n");log.flush()
 r=subprocess.run(list(map(str,argv)),env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
 log.write(r.stdout);log.flush()
 if r.returncode: raise RuntimeError("FIXTURE_COMMAND_FAILED:"+str(r.returncode))
 return r.stdout
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument("--output",required=True,type=Path);a=p.parse_args()
 out=a.output.resolve()
 if out.exists() or out.is_relative_to(ROOT):p.error("output must be fresh and outside source root")
 before={str(x):digest(x) for x in SOURCES}
 env=dict(os.environ);env["ASAN_OPTIONS"]="detect_leaks=0:halt_on_error=1";env["UBSAN_OPTIONS"]="halt_on_error=1"
 cc=shlex.split(env.get("CC","clang"));cxx=shlex.split(env.get("CXX","clang++"))
 out.mkdir(parents=True);(out/"meshtastic").mkdir()
 for h in PROTO.glob("*.pb.h"):(out/"meshtastic"/h.name).write_bytes(h.read_bytes())
 vendor=ROOT/"zephcore/adapters/bench/vendor";mono=ROOT/"zephcore/lib/monocypher"
 flags=["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I"+str(APP/"src"),
 "-I"+str(APP/"tests"),"-I"+str(vendor),"-I"+str(mono),"-I"+str(out),"-I"+str(NANOPB)]
 executions=[]
 for label,short in (("default",[]),("short-enums",["-fshort-enums"])):
  folder=out/label;folder.mkdir();objs=[]
  with (folder/"build.log").open("x") as log:
   for i,src in enumerate((NANOPB/"pb_common.c",NANOPB/"pb_encode.c",NANOPB/"pb_decode.c")):
    obj=folder/f"nanopb-{i}.o";run([*cc,"-std=c11","-Wall","-Wextra","-Werror",*flags,*short,"-c",src,"-o",obj],env,log);objs.append(obj)
   for label2,src,compiler,standard,extra in (("Proto",APP/"src/Protobuf.cpp",cxx,"c++17",["-DCONFIG_MT_GPIO_MODULES"]),("Test",TEST,cxx,"c++17",[])):
    obj=folder/(label2+".o");run([*compiler,"-std="+standard,"-Wall","-Wextra","-Werror",*flags,*short,*extra,"-c",src,"-o",obj],env,log);objs.append(obj)
   binary=folder/"m138_gpio_runtime_tests";run([*cxx,*flags,*short,*objs,"-o",binary],env,log)
   stdout=run([binary],env,log)
   executions.append({"variant":label,"status":"PASS","pass_groups":sum(x.startswith("PASS ") for x in stdout.splitlines()),"binary_sha256":digest(binary),"log_sha256":digest(folder/"build.log")})
 after={str(x):digest(x) for x in SOURCES}
 if before!=after:raise RuntimeError("SOURCE_CHANGED_DURING_FIXTURE")
 result={"schema":"zt-m138-gpio-runtime-native-result/v1","status":"PASS","source_inputs":before,"source_stable":True,"executions":executions,"GPIO_hardware":False,"radio":False,"network":False}
 (out/"RESULT.json").write_text(json.dumps(result,sort_keys=True,indent=2)+"\n")
 print(json.dumps({"status":"PASS","executions":len(executions),"groups_per_variant":executions[0]["pass_groups"],"ASAN_UBSAN":True,"GPIO_hardware":False,"radio":False,"network":False}))
if __name__=="__main__":main()
