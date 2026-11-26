#!/usr/bin/env python3
"""Build pinned PSA and run offline stock differential/fault/session tests. No devices."""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--tf-psa',required=True,type=Path,help='Clean pinned TF-PSA-Crypto dependency checkout')
p.add_argument('--output',required=True,type=Path)
p.add_argument('--ninja',type=Path,help='Optional Ninja executable')
a=p.parse_args();dep=a.tf_psa.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
pin=next(x for x in json.loads((ROOT/'mt_only/dependency-lock.json').read_text()) if x['path']=='modules/crypto/tf-psa-crypto')['commit']
actual=subprocess.check_output(['git','-C',str(dep),'rev-parse','HEAD'],text=True).strip()
if actual!=pin:raise RuntimeError('TF-PSA-Crypto revision does not match dependency-lock.json')
subprocess.run(['git','-C',str(dep),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
config=HERE/'psa-host-config.h';build=out/'psa-library';commands=[]
env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'}
def run(cmd,log):
    commands.append(cmd)
    with (out/log).open('w') as f:r=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT)
    if r.returncode:
        print((out/log).read_text()[-8000:]);raise SystemExit(r.returncode)
ninja=str(a.ninja.resolve()) if a.ninja else shutil.which('ninja')
cmd=['cmake','-S',str(dep),'-B',str(build),'-G','Ninja' if ninja else 'Unix Makefiles']
if ninja:cmd+=['-DCMAKE_MAKE_PROGRAM='+ninja]
cmd+=['-DENABLE_TESTING=OFF','-DENABLE_PROGRAMS=OFF','-DGEN_FILES=OFF',
      '-DTF_PSA_CRYPTO_CONFIG_FILE='+str(config),'-DCMAKE_BUILD_TYPE=Debug',
      '-DCMAKE_C_FLAGS=-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer']
run(cmd,'psa-configure.log');run(['cmake','--build',str(build),'--parallel','4'],'psa-build.log')
lib=build/'core/libtfpsacrypto.a'
run([sys.executable,str(ROOT/'mt_only/experimental/pki/test.py'),'--output',str(out/'differential'),
     '--psa-root',str(dep),'--psa-library',str(lib),'--psa-config',str(config)],'tests.log')
results=json.loads((out/'differential/results.json').read_text())
results['psa_source_revision']=pin;results['psa_library_sha256']=hashlib.sha256(lib.read_bytes()).hexdigest()
results['psa_host_config_sha256']=hashlib.sha256(config.read_bytes()).hexdigest()
results['persistent_storage_enabled']=False;results['new_credentials']=False
results['host_rng']='always returns insufficient entropy; no fallback'
results['component_zeroize_calls']='instrumented calls invoke real Monocypher wipe and assert zero afterwards'
(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'RESULTS.json').write_text(json.dumps(results,indent=2)+'\n')
print((out/'tests.log').read_text());print('PASS pinned PSA backend/session suite; no device access')
