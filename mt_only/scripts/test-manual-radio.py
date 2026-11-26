#!/usr/bin/env python3
"""Current Config/manual-radio and actual prior-format fixtures; host/fake I/O only."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess

p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path)
p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve()
out.mkdir(parents=True,exist_ok=False)
fixture=app/'tests/manual_radio';manifest=json.loads((fixture/'PROVENANCE.json').read_bytes())
for name,row in manifest['files'].items():
    raw=(fixture/name).read_bytes()
    assert len(raw)==row['bytes'] and hashlib.sha256(raw).hexdigest()==row['sha256']
    shutil.copyfile(fixture/name,out/name)
inputs=[app/'tests/manual_radio_cases.cpp',app/'tests/manual_radio_pure_cases.cpp',*(app/'src'/name for name in
    ('ManualLoRa.h','RadioSettings.h','USChannel.h','Config.h','SettingsJournal.h','PskNormalizer.h'))]
inputs += sorted(fixture.glob('*.inc'))+[fixture/'QWEN-ADAPTATIONS.json']
pins={str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in inputs}
commands=[];groups=[]
for label,source,expected in [('migration',inputs[0],9),('pure',inputs[1],6)]:
    cmd=[*shlex.split(os.environ.get('CXX','clang++')),'-std=c++17','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-g',*(['-fshort-enums']if a.short_enums else[]),
        '-I'+str(app/'src'),'-I'+str(app.parent/'zephcore/adapters/bench/vendor'),
        '-I'+str(app.parent/'zephcore/lib/monocypher'),str(source),'-o',str(out/(label+'-tests'))]
    subprocess.run(cmd,check=True);commands.append(cmd)
    result=subprocess.run([str(out/(label+'-tests'))],cwd=out,capture_output=True,text=True,
        env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
    (out/(label+'.log')).write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='')
    result.check_returncode();actual=[s for s in result.stdout.splitlines()if s.startswith('PASS ')]
    assert len(actual)==expected;groups+=actual
assert all(hashlib.sha256(Path(n).read_bytes()).hexdigest()==h for n,h in pins.items())
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','groups':groups,'commands':commands,
    'source_pins':pins,'short_enums':a.short_enums,'ASAN_UBSAN':True,
    'scope':'Current Config and real Config13/Journal4 migration; manual-radio components; fake storage, no firmware or RF',
    'hardware':False,'RF':False},indent=2)+'\n')
