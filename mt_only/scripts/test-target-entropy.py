#!/usr/bin/env python3
"""Offline bounded entropy state/target-branch tests; optional existing-build ABI probes."""
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path)
p.add_argument('--t096-commands',type=Path);p.add_argument('--xiao-commands',type=Path)
p.add_argument('--zephyr-root',type=Path);p.add_argument('--espressif-root',type=Path);p.add_argument('--nordic-root',type=Path);a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(root));import host_test_toolchain as host
(out/'results.json').unlink(missing_ok=True)
proto=root/'zephcore/bench/upstream-differential/source';test=app/'tests/entropy';src=app/'src/pki'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
def hashes():
    files=[src/x for x in ['BoundedEntropy.h','BoundedEntropy.cpp','ZephyrEntropy.h','ZephyrEntropy.cpp']]+sorted(test.rglob('*'))+[Path(__file__).resolve()]
    return {str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in files if x.is_file()}
before=hashes();commands=[];reports={}
verified=[]
source_evidence=[]
dep_roots={'zephyr':a.zephyr_root,'modules/hal/espressif':a.espressif_root,'modules/hal/nordic':a.nordic_root}
source_lock=json.loads((test/'source-lock.json').read_text())
dependency_lock=json.loads((app/'dependency-lock.json').read_text())
lock_by_path={item['path']:item for item in dependency_lock}
for dep in source_lock:
    dep_root=dep_roots[dep['dependency']]
    if not dep_root:continue
    dep_root=dep_root.resolve()
    declaration=lock_by_path.get(dep['dependency'])
    assert declaration is not None and declaration.get('commit')==dep['commit'] and declaration.get('url')==dep['url'],dep['dependency']
    top=subprocess.run(['git','-C',str(dep_root),'rev-parse','--show-toplevel'],text=True,capture_output=True)
    exact_git_root=top.returncode==0 and Path(top.stdout.strip()).resolve()==dep_root
    if exact_git_root:
        # A real checkout at the dependency root must match the pin exactly;
        # do not fall back to byte-only evidence after a Git mismatch.
        head=subprocess.check_output(['git','-C',str(dep_root),'rev-parse','HEAD'],text=True).strip()
        assert head==dep['commit'],dep['dependency']
        method='exact_git_root_commit_and_scoped_file_hashes'
    else:
        # Retained SDK exports are not independent Git checkouts. Their
        # identity is the matching repository lock declaration plus the
        # existing narrow entropy-source byte pins below.
        method='dependency_lock_declaration_and_scoped_file_hashes'
    for item in dep['files']:assert hashlib.sha256((dep_root/item['path']).read_bytes()).hexdigest()==item['sha256'],item['path']
    verified.append(dep['dependency'])
    source_evidence.append({'dependency':dep['dependency'],'commit':dep['commit'],'method':method,
        'scoped_file_hash_count':len(dep['files']),'independent_git_commit_proof':exact_git_root})
flags=['-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fno-exceptions','-fno-rtti','-fsanitize=address,undefined',
 *[f'-I{x}' for x in [src,app/'src',app/'experimental/pki',root/'zephcore/adapters/bench/vendor',root/'zephcore/lib/monocypher',out,proto/'nanopb',test/'stubs']],'-Dcrypto_wipe=mt_entropy_test_wipe']
variants={'core':[], 't096':['CONFIG_SOC_NRF52840','CONFIG_ENTROPY_NRF5_RNG','CONFIG_ENTROPY_NRF5_BIAS_CORRECTION'],
 'xiao':['CONFIG_SOC_SERIES_ESP32S3','CONFIG_BT'],
 'xiao-pm-refused':['CONFIG_SOC_SERIES_ESP32S3','CONFIG_BT','CONFIG_PM','MT_TEST_UNSUPPORTED'],
 'xiao-smp-refused':['CONFIG_SOC_SERIES_ESP32S3','CONFIG_BT','CONFIG_SMP','MT_TEST_UNSUPPORTED'],
 'xiao-no-bt-refused':['CONFIG_SOC_SERIES_ESP32S3','MT_TEST_UNSUPPORTED'],
 'nrf-no-bias-refused':['CONFIG_SOC_NRF52840','CONFIG_ENTROPY_NRF5_RNG','MT_TEST_UNSUPPORTED']}
for name,defs in variants.items():
    sources=[src/'BoundedEntropy.cpp',test/('entropy_tests.cpp' if name=='core' else 'target_tests.cpp')]
    if name!='core':sources.append(src/'ZephyrEntropy.cpp')
    cmd=[*host.cxx(),*flags,*['-D'+v+'=1' for v in defs],*map(str,sources),'-o',str(out/name)]
    commands.append(cmd);subprocess.run(cmd,check=True)
    r=subprocess.run([str(out/name)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
    (out/(name+'.log')).write_text(r.stdout+r.stderr);print(name+':\n'+r.stdout+r.stderr);r.check_returncode()
    reports[name]={'status':'PASS','groups':sum(x.startswith('PASS ') for x in r.stdout.splitlines())}
probes={}
for name,path in [('t096',a.t096_commands),('xiao',a.xiao_commands)]:
    if not path:continue
    entries=json.loads(path.read_text());entry=next(x for x in entries if x['file'].endswith('/mt_only/src/main.cpp'))
    args=entry.get('arguments') or shlex.split(entry['command']);base=[];skip=False
    for x in args:
        if skip:skip=False;continue
        if x=='-o':skip=True;continue
        if x=='-c' or x==entry['file']:continue
        base.append(x)
    probe=out/(name+'-target');probe.mkdir(exist_ok=True)
    records=[]
    for unit in ['BoundedEntropy','ZephyrEntropy']:
        obj=probe/(unit+'.o');cmd=[*base,f'-I{app / "experimental/pki"}','-Werror','-fstack-usage','-c',str(src/(unit+'.cpp')),'-o',str(obj)]
        commands.append(cmd);r=subprocess.run(cmd,cwd=entry['directory'],text=True,capture_output=True)
        (probe/(unit+'.log')).write_text(r.stdout+r.stderr);r.check_returncode()
        records.append({'unit':unit,'object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest(),'stack_usage':obj.with_suffix('.su').read_text().splitlines()})
    probes[name]={'status':'COMPILE_ONLY','compile_commands_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'objects':records}
assert before==hashes(),'Source changed during validation'
(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'variants':reports,'target_probes':probes,'source_stable':True,'source_sha256':before,'verified_pinned_sources':verified,'source_pin_evidence':source_evidence,'hardware':False,'hardware_entropy_quality':'not tested'},indent=2)+'\n')
