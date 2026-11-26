#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Native memory-only notification record/journal adversaries; no hardware/network."""
if not __debug__:raise RuntimeError('Optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess
app=Path(__file__).resolve().parents[1];root=app.parent
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path);a=parser.parse_args();out=a.output.resolve()
if out.exists()or out.is_relative_to(root):parser.error('fresh output outside source required')
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
sha=lambda raw:hashlib.sha256(raw).hexdigest()
out.mkdir(parents=True);(out/'meshtastic').mkdir()
for path in proto.glob('*.pb.h'):shutil.copyfile(path,out/'meshtastic'/path.name)
flags=['-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
cxx=shlex.split(os.environ.get('CXX','clang++'));rows=[]
result={'status':'FAIL','hardware':False,'network':False,'memory_backend_only':True,'ASAN_UBSAN':True,'variants':rows,'source_stable':False,'script_sha256':sha(Path(__file__).read_bytes())}
def original(path):
    path=path.resolve()
    if path.parent==out/'meshtastic':
        source=proto/path.name;assert path.read_bytes()==source.read_bytes();return source
    return path
def dependencies(dep):
    # Compiler make dependencies use escaped spaces and backslash-newlines.
    text=dep.read_text().replace('\\\n',' ');return [Path(n)for n in shlex.split(text.split(':',1)[1])]
def pins(paths):
    result={}
    for p in paths:
        source=original(p);name=str(source.relative_to(root))if source.is_relative_to(root)else str(source)
        result[name]=sha(source.read_bytes())
    return result
try:
    version=subprocess.run([*cxx,'--version'],capture_output=True,text=True,check=True,timeout=15).stdout
    (out/'compiler.txt').write_text(version);result['compiler_version_sha256']=sha(version.encode())
    for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
        for suite,groups in [('settings',6),('journal',11),('ztn2',1)]:
            name=label+'-'+suite;source=app/'tests'/('notification_'+suite+'_tests.cpp');dep=out/(name+'.d')
            dependency_cmd=[*cxx,*flags,*extra,'-MM','-MF',str(dep),'-MT',name,str(source)]
            with(out/(name+'.dependencies.log')).open('x')as log:subprocess.run(dependency_cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
            selected=dependencies(dep);before=pins(selected)
            cmd=[*cxx,*flags,*extra,'-MMD','-MF',str(dep),str(source),'-o',str(out/name)]
            with(out/(name+'.build.log')).open('x')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
            assert pins(dependencies(dep))==before
            execute=[str(out/name)]
            with(out/(name+'.test.log')).open('x')as log:subprocess.run(execute,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            lines=(out/(name+'.test.log')).read_text().splitlines();assert sum(line.startswith('PASS ')for line in lines)==groups and lines[-1]=='NOTIFICATION_'+suite.upper()+'_GROUPS '+str(groups)
            assert pins(selected)==before
            rows.append({'variant':label,'suite':suite,'groups':groups,'dependency_command':dependency_cmd,'compile_command':cmd,'execution_command':execute,'compiled_input_sha256':before,'binary_sha256':sha((out/name).read_bytes()),'test_log_sha256':sha((out/(name+'.test.log')).read_bytes())})
    assert len(rows)==6 and sha(Path(__file__).read_bytes())==result['script_sha256']
    for row in rows:assert all(sha((root/n).read_bytes())==digest for n,digest in row['compiled_input_sha256'].items())
    result.update(status='PASS',source_stable=True,settings_groups_per_enum=6,journal_groups_per_enum=11,ztn2_groups_per_enum=1)
finally:(out/'results.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
print(json.dumps({'status':result['status'],'executions':len(rows),'settings_groups_per_enum':6,'journal_groups_per_enum':11,'ztn2_groups_per_enum':1,'ASAN_UBSAN':True,'hardware':False,'network':False}))
