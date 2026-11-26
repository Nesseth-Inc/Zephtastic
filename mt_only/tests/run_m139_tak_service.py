#!/usr/bin/env python3
"""Inert-by-default host test for the actual M139 Service TAK announcement path."""
import argparse, hashlib, json, os, platform, shlex, shutil, subprocess
from pathlib import Path

HERE=Path(__file__).resolve().parent
APP=HERE.parent
ROOT=APP.parent
PROTO=ROOT/'zephcore/bench/upstream-differential/source'
NANOPB=PROTO/'nanopb'
VENDOR=ROOT/'zephcore/adapters/bench/vendor'
MONO=ROOT/'zephcore/lib/monocypher'
CASE=HERE/'m139_tak_service_tests.cpp'
RUNNER=Path(__file__).resolve()

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def run(argv,env,log):
    log.write('$ '+shlex.join(map(str,argv))+'\n');log.flush()
    r=subprocess.run(list(map(str,argv)),env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    log.write(r.stdout);log.flush()
    if r.returncode: raise RuntimeError('COMMAND_FAILED:'+str(r.returncode))
    return r.stdout

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',action='store_true');p.add_argument('--source',type=Path,default=ROOT)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    if not a.run:p.error('inert by default; pass --run')
    repo=a.source.resolve();app=repo/'mt_only';proto=repo/'zephcore/bench/upstream-differential/source'
    vendor=repo/'zephcore/adapters/bench/vendor';mono=repo/'zephcore/lib/monocypher';case=app/'tests/m139_tak_service_tests.cpp';runner=Path(__file__).resolve()
    out=a.output.resolve()
    if out.exists() or out.is_relative_to(repo):p.error('output must be fresh and outside source tree')
    sources=[case,runner,app/'tests/endpoint_fixture.h',app/'tests/known_channel_fixture.h',
      app/'src/Service.h',app/'src/Endpoint.h',app/'src/Companion.h',app/'src/Config.h',
      app/'src/Discovery.h',app/'src/PositionQuery.h',app/'src/PublicApplication.h',
      app/'src/DeviceRolePolicy.h',app/'src/modules/TakPliPayload.h',app/'src/pki/PrivateRuntime.h',
      app/'src/HardwareSnapshot.h',app/'src/Protobuf.cpp',vendor/'MtCodec.h',vendor/'MtCodec.cpp',
      vendor/'MtSignature.h',vendor/'MtSignature.cpp',*proto.glob('*.pb.h'),
      *[NANOPB/x for x in ('pb.h','pb_common.h','pb_encode.h','pb_decode.h','pb_common.c','pb_encode.c','pb_decode.c')],
      MONO/'monocypher.c',MONO/'monocypher-ed25519.c']
    before={str(x.relative_to(repo)):sha(x) for x in sources}
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'}
    cc=shlex.split(env.get('CC','clang'));cxx=shlex.split(env.get('CXX','clang++'))
    out.mkdir(parents=True);gen=out/'generated'/'meshtastic';gen.mkdir(parents=True)
    for h in proto.glob('*.pb.h'):shutil.copyfile(h,gen/h.name)
    flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DCONFIG_MT_TAK_TRACKER=1',
      '-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out/'generated'),'-I'+str(NANOPB)]
    host_link=[]
    if platform.system()=='Linux':flags+=['-DMT_HOST_OPENSSL=1'];host_link=['-lcrypto']
    variants=[]
    for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
      d=out/label;d.mkdir();objects=[]
      with (d/'build.log').open('x') as log:
        for i,src in enumerate([NANOPB/'pb_common.c',NANOPB/'pb_encode.c',NANOPB/'pb_decode.c',MONO/'monocypher.c',MONO/'monocypher-ed25519.c']):
          obj=d/f'c-{i}.o';run([*cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,*extra,'-c',src,'-o',obj],env,log);objects.append(obj)
        for i,src in enumerate([app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',case]):
          obj=d/f'cpp-{i}.o';unused=['-Wno-unused-function'] if i==3 else []
          run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*unused,*flags,*extra,'-c',src,'-o',obj],env,log);objects.append(obj)
        binary=d/'m139-tak-service';run([*cxx,*flags,*extra,*objects,*host_link,'-o',binary],env,log)
        stdout=run([binary],env,log)
      variants.append({'variant':label,'status':'PASS','groups':sum(x.startswith('PASS ') for x in stdout.splitlines()),
                       'binary_sha256':sha(binary),'log_sha256':sha(d/'build.log'),'stdout':stdout.strip()})
    after={str(x.relative_to(repo)):sha(x) for x in sources}
    if before!=after:raise RuntimeError('SOURCE_CHANGED_DURING_TEST')
    result={'schema':'m139-tak-service-native/v1','status':'PASS','source_stable':True,'source_sha256':before,
      'variants':variants,'ASAN_UBSAN':True,'target_build':False,'hardware':False,'RF':False,'network':False}
    (out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    print(json.dumps({'status':'PASS','variants':len(variants),'groups_each':variants[0]['groups'],'RF':False}))
if __name__=='__main__':main()
