#!/usr/bin/env python3
"""Synthetic-only provisioning core fault tests; no device or network access."""
import argparse,hashlib,json,os,pathlib,shlex,subprocess
HERE=pathlib.Path(__file__).resolve().parent;root=HERE.parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--cases',type=pathlib.Path,default=HERE/'pki_provisioning_tests.cpp');a=p.parse_args();cases=a.cases.resolve()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);(out/'RESULTS.json').unlink(missing_ok=True)
mono=root/'zephcore/lib/monocypher';vendor=root/'zephcore/adapters/bench/vendor'
inputs=[f for d in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,vendor) for f in sorted(d.rglob('*')) if f.is_file() and f.suffix in ('.h','.c','.cpp')]
inputs += [cases,pathlib.Path(__file__).resolve()]
def hashes():return {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
before=hashes();flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',*['-I'+str(d) for d in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,vendor)]]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
commands=[];objects=[]
def run(command):
    commands.append(command);r=subprocess.run(command,capture_output=True,text=True)
    with (out/'compile.log').open('a') as log:log.write(r.stdout+r.stderr)
    if r.returncode:print(r.stdout+r.stderr);raise SystemExit(r.returncode)
(out/'compile.log').write_text('')
for source in (mono/'monocypher.c',mono/'monocypher-ed25519.c'):
    obj=out/(source.name+'.o');run([*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)]);objects.append(str(obj))
sources=[cases,*[root/f'mt_only/src/pki/{name}.cpp' for name in ('PkiIdentity','PkiLedger','PkiStorage','PkiAppendHistory','PkiProvisioning')],vendor/'MtSignature.cpp']
run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,*map(str,sources),*objects,'-o',str(out/'tests')])
r=subprocess.run([str(out/'tests')],capture_output=True,text=True,timeout=240,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
print(r.stdout+r.stderr,end='');(out/'run.log').write_text(r.stdout+r.stderr)
stable=hashes()==before
(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'RESULTS.json').write_text(json.dumps({'status':'PASS' if not r.returncode and stable else 'FAIL','exit':r.returncode,'source_stable':stable,'source_sha256':before,'ASAN_UBSAN':True,'hardware':False,'credentials':'public deterministic host fixtures only'},indent=2)+'\n')
raise SystemExit(r.returncode if stable else 2)
