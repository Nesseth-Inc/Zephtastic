#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Inert-by-default ASan/UBSan tests for strict bounded TAK reply validation."""
import argparse, hashlib, json, os, shlex, subprocess
from pathlib import Path

HERE=Path(__file__).resolve().parent
APP=HERE.parent
WT=APP.parent
SOURCE=WT/'zephcore/bench/upstream-differential/source'
NANOPB=SOURCE/'nanopb'
VENDOR=WT/'zephcore/adapters/bench/vendor'
INPUTS=[APP/'src/modules/TakPliReplyValidation.h',APP/'src/modules/TakPliReplyPayload.h',
        APP/'src/modules/TakPliPayload.h',VENDOR/'MtCodec.h',APP/'src/Protobuf.cpp',HERE/'m140_tak_reply_validation_tests.cpp',
        Path(__file__).resolve(),SOURCE/'source-lock.json',*sorted(SOURCE.glob('*.pb.h')),
        *[NANOPB/n for n in ('pb.h','pb_common.h','pb_encode.h','pb_decode.h','pb_common.c','pb_encode.c','pb_decode.c')]]
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(argv,env,log):
    log.write('$ '+shlex.join(map(str,argv))+'\n');log.flush()
    r=subprocess.run(list(map(str,argv)),cwd=WT,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    log.write(r.stdout);log.flush()
    if r.returncode:raise RuntimeError('FIXTURE_COMMAND_FAILED:'+str(r.returncode))
    return r.stdout
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',action='store_true');p.add_argument('--output',required=True,type=Path);a=p.parse_args()
    if not a.run:p.error('inert by default; pass --run')
    out=a.output.resolve()
    if out.exists() or out.is_relative_to(WT):p.error('output must be fresh and outside source worktree')
    before={str(x):digest(x) for x in INPUTS};env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0:halt_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1'
    cc=shlex.split(env.get('CC','clang'));cxx=shlex.split(env.get('CXX','clang++'));out.mkdir(parents=True)
    inc=out/'include/meshtastic';inc.parent.mkdir();inc.symlink_to(SOURCE,target_is_directory=True)
    common=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(out/'include'),'-I'+str(NANOPB),'-I'+str(APP/'src'),'-I'+str(VENDOR)]
    runs=[]
    for label,enum_flags in (('default',[]),('short-enums',['-fshort-enums'])):
        d=out/label;d.mkdir();objects=[]
        with (d/'build.log').open('x') as log:
            for i,name in enumerate(('pb_common.c','pb_encode.c','pb_decode.c')):
                obj=d/f'pb-{i}.o';run([*cc,'-std=c11','-Wall','-Wextra','-Werror',*common,*enum_flags,'-c',NANOPB/name,'-o',obj],env,log);objects.append(obj)
            for i,src in enumerate((APP/'src/Protobuf.cpp',HERE/'m140_tak_reply_validation_tests.cpp')):
                obj=d/f'cpp-{i}.o';run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',*common,*enum_flags,'-c',src,'-o',obj],env,log);objects.append(obj)
            binary=d/'validation-tests';run([*cxx,*common,*enum_flags,*objects,'-o',binary],env,log);stdout=run([binary],env,log)
        runs.append({'variant':label,'status':'PASS','stdout':stdout.strip(),'binary_sha256':digest(binary),'log_sha256':digest(d/'build.log')})
    after={str(x):digest(x) for x in INPUTS}
    if before!=after:raise RuntimeError('SOURCE_CHANGED_DURING_TEST')
    result={'schema':'m140-tak-reply-validation-native/v1','status':'PASS','input_sha256':before,'source_stable':True,'executions':runs,'ASAN_UBSAN':True,'target_build':False,'hardware':False,'RF':False,'network':False}
    (out/'RESULT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    print(json.dumps({'status':'PASS','variants':len(runs),'ASAN_UBSAN':True}))
if __name__=='__main__':main()
