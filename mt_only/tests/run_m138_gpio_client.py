#!/usr/bin/env python3
"""Compile/run exact Companion + Service GPIO client and RX-correlation tests."""
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
APP=Path(__file__).resolve().parents[1];ROOT=APP.parent
PROTO=ROOT/"zephcore/bench/upstream-differential/source";NANOPB=PROTO/"nanopb"
VENDOR=ROOT/"zephcore/adapters/bench/vendor";MONO=ROOT/"zephcore/lib/monocypher"
TEST=APP/"tests/m138_gpio_client_tests.cpp";VECTORS=APP/"tests/m138_gpio_sdk_vectors.h";RUNNER=Path(__file__).resolve()
INPUTS=[TEST,VECTORS,RUNNER,APP/"src/Companion.h",APP/"src/Service.h",APP/"src/PublicApplication.h",
 APP/"src/Endpoint.h",APP/"src/modules/GpioModuleWire.h",APP/"src/modules/GpioModuleChannels.h",
 APP/"src/pki/PrivateRuntime.h",VENDOR/"MtCodec.h",VENDOR/"MtCodec.cpp",
 VENDOR/"MtSignature.h",VENDOR/"MtSignature.cpp",APP/"src/Protobuf.cpp",
 *[PROTO/name for name in ("mesh.pb.h","remote_hardware.pb.h","portnums.pb.h","admin.pb.h","module_config.pb.h")],
 *[NANOPB/name for name in ("pb.h","pb_common.h","pb_encode.h","pb_decode.h","pb_common.c","pb_encode.c","pb_decode.c")],
 MONO/"monocypher.c",MONO/"monocypher-ed25519.c"]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(argv,env,log):
 log.write("$ "+shlex.join(map(str,argv))+"\n");log.flush()
 p=subprocess.run(list(map(str,argv)),env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=240)
 log.write(p.stdout);log.flush()
 if p.returncode:raise RuntimeError("FIXTURE_COMMAND_FAILED:"+str(p.returncode))
 return p.stdout
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument("--output",required=True,type=Path);a=ap.parse_args()
 out=a.output.resolve()
 if out.exists() or out.is_relative_to(ROOT):ap.error("output must be fresh and outside source root")
 before={str(x):sha(x) for x in INPUTS};env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",UBSAN_OPTIONS="halt_on_error=1")
 cc=shlex.split(env.get("CC","clang"));cxx=shlex.split(env.get("CXX","clang++"));host=[]
 common=["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer","-DCONFIG_MT_GPIO_MODULES=1",
  "-I"+str(APP/"src"),"-I"+str(APP/"tests"),"-I"+str(VENDOR),"-I"+str(MONO),"-I"+str(out),"-I"+str(NANOPB)]
 if sys.platform.startswith("linux"):common.append("-DMT_HOST_OPENSSL=1");host.append("-lcrypto")
 out.mkdir(parents=True);generated=out/"meshtastic";generated.mkdir()
 for h in PROTO.glob("*.pb.h"):shutil.copyfile(h,generated/h.name)
 executions=[]
 for label,enum_flags in (("default",[]),("short-enums",["-fshort-enums"])):
  folder=out/label;folder.mkdir();objects=[]
  with (folder/"build.log").open("x") as log:
   for i,src in enumerate((NANOPB/"pb_common.c",NANOPB/"pb_encode.c",NANOPB/"pb_decode.c",MONO/"monocypher.c",MONO/"monocypher-ed25519.c")):
    obj=folder/("c-"+str(i)+".o");run([*cc,"-std=c11","-Wall","-Wextra","-Werror",*common,*enum_flags,"-c",src,"-o",obj],env,log);objects.append(obj)
   for name,src in (("proto",APP/"src/Protobuf.cpp"),("codec",VENDOR/"MtCodec.cpp"),("signature",VENDOR/"MtSignature.cpp"),("fixture",TEST)):
    obj=folder/(name+".o");run([*cxx,"-std=c++17","-Wall","-Wextra","-Werror",*common,*enum_flags,"-c",src,"-o",obj],env,log);objects.append(obj)
   binary=folder/"m138_gpio_client_tests";run([*cxx,*common,*enum_flags,*objects,*host,"-o",binary],env,log)
   stdout=run([binary],env,log)
   executions.append({"variant":label,"status":"PASS","binary_sha256":sha(binary),"log_sha256":sha(folder/"build.log"),"test_output":stdout.strip()})
 after={str(x):sha(x) for x in INPUTS}
 if before!=after:raise RuntimeError("SOURCE_CHANGED_DURING_FIXTURE")
 result={"schema":"zt-m138-gpio-client-native-result/v1","status":"PASS","source_stable":True,"input_sha256":before,"executions":executions,"ASAN_UBSAN":True,"hardware":False,"RF":False,"network":False,"target_build":False}
 (out/"RESULT.json").write_text(json.dumps(result,sort_keys=True,indent=2)+"\n");print(json.dumps({"status":"PASS","variants":len(executions),"source_stable":True}))
if __name__=="__main__":main()
