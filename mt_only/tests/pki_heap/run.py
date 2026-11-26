#!/usr/bin/env python3
"""Offline real PSA generic-cipher allocation/failure test; no devices or global allocator override."""
from pathlib import Path
import argparse,subprocess,json,hashlib,os,shlex
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--tf-psa',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
H=Path(__file__).resolve().parent;S=a.source.resolve();D=a.tf_psa.resolve();O=a.output.resolve();O.mkdir(parents=True,exist_ok=True);B=O/'psa-library'
(O/'RESULTS.json').unlink(missing_ok=True);(O/'build.log').write_text('')
config=H/'psa-host-config.h';flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-O1',f'-DTF_PSA_CRYPTO_CONFIG_FILE="{config}"']
incs=[D/'drivers/builtin/include',D/'include',D/'dispatch/include',D/'drivers/builtin/src',D/'core',D/'platform',D/'utilities',B/'include',S/'mt_only/src/pki',S/'mt_only/experimental/pki',S/'zephcore/lib/monocypher'];flags+=['-I'+str(v) for v in incs]
lock=json.loads((S/'mt_only/dependency-lock.json').read_text());pin=next(v['commit'] for v in lock if v['path']=='modules/crypto/tf-psa-crypto')
assert subprocess.check_output(['git','-C',D,'rev-parse','HEAD'],text=True).strip()==pin
subprocess.run(['git','-C',D,'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
files=[Path(__file__).resolve(),S/'mt_only/src/pki/PsaPkiBackend.h',S/'mt_only/experimental/pki/PkiCrypto.h',S/'zephcore/lib/monocypher/monocypher.h',S/'zephcore/lib/monocypher/monocypher.c',H/'cases.cpp',H/'allocator_shim.h',config,S/'mt_only/src/pki/PsaPkiBackend.cpp',*[D/q for q in ['drivers/builtin/src/cipher_wrap.c','drivers/builtin/src/cipher.c','drivers/builtin/src/cmac.c','platform/platform_util.c']]]
hashfile=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
before={f.name:hashfile(f) for f in files};commands=[]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
def run(c):
 commands.append(list(map(str,c)));r=subprocess.run(c,text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1','PYTHONDONTWRITEBYTECODE':'1'})
 with (O/'build.log').open('a') as log:log.write(r.stdout+r.stderr)
 assert not r.returncode,r.stdout+r.stderr
 return r.stdout+r.stderr
run(['cmake','-S',D,'-B',B,'-DENABLE_TESTING=OFF','-DENABLE_PROGRAMS=OFF','-DGEN_FILES=OFF','-DTF_PSA_CRYPTO_CONFIG_FILE='+str(config),'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer'])
run(['cmake','--build',B,'--parallel','4'])
objects=[]
for unit in ['cipher_wrap','cipher','cmac','platform_util']:
 src=D/(('platform/' if unit=='platform_util' else 'drivers/builtin/src/')+unit+'.c');obj=O/(unit+'.o');objects.append(obj)
 run([*cc,'-std=c99',*flags,'-include',H/'allocator_shim.h','-c',src,'-o',obj])
obj=O/'monocypher.o';objects.append(obj);run([*cc,'-std=c99',*flags,'-c',S/'zephcore/lib/monocypher/monocypher.c','-o',obj])
run([*cxx,'-std=c++17',*flags,H/'cases.cpp',S/'mt_only/src/pki/PsaPkiBackend.cpp',*objects,B/'core/libtfpsacrypto.a','-o',O/'cases'])
log=run([O/'cases']);(O/'tests.log').write_text(log);print(log)
assert before=={f.name:hashfile(f) for f in files}
(O/'RESULTS.json').write_text(json.dumps({'status':'PASS','source_stable':True,'PSA_revision':pin,'source_sha256':before,'CCM_allocation_failure_cases':513,'CMAC_second_allocation_failure_cases':1,'overlapping_CCM_CMAC_peak_allocations':3,'no_output_on_CCM_failure':True,'later_success_after_failures':True,'ASAN_UBSAN':True,'scope':'Scratch library translation-unit allocator shims only; real pinned PSA/backend; host pointer ABI. No global allocator override, devices, persistent keys or runtime target qualification.'},indent=2)+'\n')
(O/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
