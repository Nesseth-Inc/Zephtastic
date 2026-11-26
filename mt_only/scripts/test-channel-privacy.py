#!/usr/bin/env python3
"""Channel promotion/privacy/UI components, synthetic prior producer, no I/O."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
proto=app.parent/'zephcore/bench/upstream-differential/source';vendor=app.parent/'zephcore/adapters/bench/vendor';mono=app.parent/'zephcore/lib/monocypher'
refs=app/'tests/channel_privacy/v14';provenance=json.loads((refs/'PROVENANCE.json').read_bytes())
for name,row in provenance['files'].items():
 raw=(refs/name).read_bytes();assert len(raw)==row['bytes']and hashlib.sha256(raw).hexdigest()==row['sha256']
(out/'meshtastic').mkdir()
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
inputs=[Path(__file__).resolve(),app/'tests/channel_privacy_cases.cpp',app/'tests/channel_privacy/v14_producer.cpp',app/'tests/channel_privacy/precision_cases.inc',app/'tests/stock_precision_reference.h',*refs.iterdir(),*(app/'src'/n for n in('Config.h','SettingsJournal.h','ChannelOrder.h','ChannelRecord.h','PositionQuery.h','PositionPrecision.h','ui/Model.h','ui/RadioView.h','ui/RadioPresentation.h','RadioSettings.h','PrimaryChannel.hpp','Protobuf.cpp'))]
inputs.extend(app/'src/stock/position'/name for name in ('PositionPrecision.cpp','PositionPrecision.h','PositionChannels.h'))
pins={str(n):hashlib.sha256(n.read_bytes()).hexdigest()for n in inputs}
flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
if a.short_enums:flags+=['-fshort-enums']
commands=[];cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'}
def run(cmd):
 commands.append(cmd);subprocess.run(cmd,check=True,cwd=out,env=env)
run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/channel_privacy/v14_producer.cpp'),'-o',str(out/'v14-producer')]);run([str(out/'v14-producer')])
objects=[]
for name in ['pb_common.c','pb_encode.c','pb_decode.c']:
 obj=out/(name+'.o');run([*cc,'-std=c11',*flags,'-c',str(proto/'nanopb'/name),'-o',str(obj)]);objects.append(str(obj))
profiles = {
    'normal': ['-DTEST_EVENT_BLOCKED=0'],
    'event': ['-DTEST_EVENT_BLOCKED=1', '-DUSERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL=1',
              '-DUSERPREFS_CHANNEL_0_PSK={0xa5,0x1e,0x7c,0,0,0,0,0,0,0,0,0,0,0,0,0}'],
}
profile_groups = {}
for profile, definitions in profiles.items():
    exe = out / ('channel-privacy-tests' if profile == 'normal' else 'channel-privacy-event-tests')
    run([*cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags, *definitions,
         str(app/'tests/channel_privacy_cases.cpp'), str(app/'src/Protobuf.cpp'), *objects, '-o', str(exe)])
    commands.append([str(exe)])
    result = subprocess.run([str(exe)], cwd=out, capture_output=True, text=True, env=env)
    log = result.stdout + result.stderr
    (out / ('test.log' if profile == 'normal' else 'test-event.log')).write_text(log)
    print(f'{profile}\n{log}', end='')
    (out/'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    result.check_returncode()
    groups = [line for line in result.stdout.splitlines() if line.startswith('PASS ')]
    assert len(groups) == 10
    profile_groups[profile] = groups
assert all(hashlib.sha256(Path(n).read_bytes()).hexdigest()==pin for n,pin in pins.items())
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','groups':profile_groups['normal'],
    'profiles':profile_groups,'source_pins':pins,'ASAN_UBSAN':True,'short_enums':a.short_enums,
    'scope':'Native channel settings, position packet bytes, prior producer and UI; no hardware or RF proof',
    'hardware':False,'RF':False},indent=2)+'\n')
