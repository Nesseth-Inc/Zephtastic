#!/usr/bin/env python3
"""Run disabled legacy-tail profile behavior on synthetic host fixtures only."""
import argparse, os, pathlib, shlex, shutil, subprocess

HERE=pathlib.Path(__file__).resolve().parent
ROOT=HERE.parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--zephyr',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args(); out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
compiler=shlex.split(os.environ.get('CXX','clang++'))
if not compiler: raise SystemExit('CXX resolved to an empty command')
compiler_path=shutil.which(compiler[0])
if not compiler_path: raise SystemExit(f'C++ compiler not found: {compiler[0]}')
wrapper=out/'cxx-disabled-profile.sh'
wrapper.write_text('#!/bin/sh\nexec '+' '.join(map(shlex.quote,[compiler_path,*compiler[1:]]))+
                   ' -DMT_PKI_LEGACY_TAIL_TOOLS_NATIVE_DISABLED=1 "$@"\n')
wrapper.chmod(0o755)
env={**os.environ,'CXX':str(wrapper)}
def run(args):
    result=subprocess.run(args,env=env)
    if result.returncode: raise SystemExit(result.returncode)
run([shutil.which('python3') or 'python3',str(HERE/'pki_storage_nvs/run.py'),
     '--zephyr',str(a.zephyr.resolve()),'--output',str(out/'storage'),
     '--cases',str(HERE/'m142_legacy_tools_disabled_tests.cpp')])
run([shutil.which('python3') or 'python3',str(HERE/'run-m12-provisioning.py'),
     '--output',str(out/'provisioning')])
print('PASS disabled legacy-tail storage fixture and retained provisioning regression suite; synthetic only')
