#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Inert-by-default native Service/Endpoint/Companion position-response fixtures."""
import argparse,hashlib,json,os,platform,shlex,shutil,subprocess
from pathlib import Path

HERE=Path(__file__).resolve().parent;APP=HERE.parent;WT=APP.parent
PROTO=WT/'zephcore/bench/upstream-differential/source';NANOPB=PROTO/'nanopb';VENDOR=WT/'zephcore/adapters/bench/vendor';MONO=WT/'zephcore/lib/monocypher'
CASE=HERE/'m140_tak_reply_integration_tests.cpp'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(argv,env,log):
    log.write('$ '+shlex.join(map(str,argv))+'\n');log.flush();r=subprocess.run(list(map(str,argv)),cwd=WT,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=240)
    log.write(r.stdout);log.flush()
    if r.returncode:raise RuntimeError('COMMAND_FAILED:'+str(r.returncode))
    return r.stdout
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',action='store_true');p.add_argument('--output',required=True,type=Path);p.add_argument('--source',type=Path,default=WT);a=p.parse_args()
    if not a.run:p.error('inert by default; pass --run')
    repo=a.source.resolve();app=repo/'mt_only';proto=repo/'zephcore/bench/upstream-differential/source';vendor=repo/'zephcore/adapters/bench/vendor';mono=repo/'zephcore/lib/monocypher';nanopb=proto/'nanopb';case=app/'tests/m140_tak_reply_integration_tests.cpp';out=a.output.resolve()
    if out.exists() or out.is_relative_to(repo):p.error('output must be fresh and outside source worktree')
    tracked=[case,Path(__file__).resolve(),app/'tests/position_query_fixture.h',app/'tests/endpoint_fixture.h',app/'tests/known_channel_fixture.h',
      app/'src/Endpoint.h',app/'src/Service.h',app/'src/Companion.h',app/'src/PositionQuery.h',app/'src/PositionPrecision.h',app/'src/Config.h',
      app/'src/modules/TakPliPayload.h',app/'src/modules/TakPliReplyPayload.h',app/'src/modules/TakPliReplyValidation.h',app/'src/Protobuf.cpp',
      vendor/'MtCodec.h',vendor/'MtCodec.cpp',vendor/'MtSignature.h',vendor/'MtSignature.cpp',*proto.glob('*.pb.h'),
      *[nanopb/n for n in ('pb.h','pb_common.h','pb_encode.h','pb_decode.h','pb_common.c','pb_encode.c','pb_decode.c')],
      MONO/'monocypher.c',MONO/'monocypher-ed25519.c']
    before={str(x.relative_to(repo)):digest(x) for x in tracked};env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'}
    cc=shlex.split(env.get('CC','clang'));cxx=shlex.split(env.get('CXX','clang++'));host=[]
    if platform.system()=='Linux':host=['-lcrypto']
    flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out/'generated'),'-I'+str(nanopb)]
    if platform.system()=='Linux':flags+=['-DMT_HOST_OPENSSL=1']
    out.mkdir(parents=True);gen=out/'generated/meshtastic';gen.mkdir(parents=True)
    for h in proto.glob('*.pb.h'):shutil.copyfile(h,gen/h.name)
    executions=[]
    for feature in (False,True):
      for label,extra in (('default',[]),('short-enums',['-fshort-enums'])):
        name=('tak-on-' if feature else 'tak-off-')+label;d=out/name;d.mkdir();objects=[];featureFlags=['-DCONFIG_MT_TAK_TRACKER=1'] if feature else []
        with (d/'build.log').open('x') as log:
          for i,src in enumerate([nanopb/'pb_common.c',nanopb/'pb_encode.c',nanopb/'pb_decode.c',mono/'monocypher.c',mono/'monocypher-ed25519.c']):
            obj=d/f'c-{i}.o';run([*cc,'-std=c11','-Wall','-Wextra','-Werror',*flags,*featureFlags,*extra,'-c',src,'-o',obj],env,log);objects.append(obj)
          for i,src in enumerate([app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',case]):
            obj=d/f'cpp-{i}.o';run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,*featureFlags,*extra,'-c',src,'-o',obj],env,log);objects.append(obj)
          binary=d/'integration-tests';run([*cxx,*flags,*featureFlags,*extra,*objects,*host,'-o',binary],env,log);stdout=run([binary],env,log)
        executions.append({'variant':name,'status':'PASS','groups':sum(x.startswith('PASS ') for x in stdout.splitlines()),'stdout':stdout.strip(),'binary_sha256':digest(binary),'log_sha256':digest(d/'build.log')})
    after={str(x.relative_to(repo)):digest(x) for x in tracked}
    if before!=after:raise RuntimeError('SOURCE_CHANGED_DURING_TEST')
    result={'schema':'m140-tak-reply-integration-native/v1','status':'PASS','source_stable':True,'source_sha256':before,'variants':executions,'ASAN_UBSAN':True,'target_build':False,'hardware':False,'RF':False,'network':False}
    (out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n');print(json.dumps({'status':'PASS','variants':len(executions),'groups_by_variant':[x['groups'] for x in executions],'RF':False}))
if __name__=='__main__':main()
