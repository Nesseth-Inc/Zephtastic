#!/usr/bin/env python3
"""Stored key expansion against literal pinned stock functions; synthetic I/O."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
proto=app.parent/'zephcore/bench/upstream-differential/source'
oracles=app/'tests/channel_key'
expected= json.loads((oracles/'PINS.json').read_bytes())
assert expected['stock_revision']=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
for name,pin in expected['functions'].items():assert hashlib.sha256((oracles/name).read_bytes()).hexdigest()==pin
files=[Path(__file__).resolve(),app/'tests/channel_key_cases.cpp',app/'src/PskNormalizer.h',app/'src/PrimaryChannel.hpp',app/'src/ChannelName.h',app/'src/Config.h',oracles/'PINS.json',*(oracles/n for n in expected['functions'])]
pins={str(f):hashlib.sha256(f.read_bytes()).hexdigest()for f in files}
(out/'meshtastic').mkdir()
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
cmd=[*shlex.split(os.environ.get('CXX','clang++')),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',*(['-fshort-enums']if a.short_enums else[]),'-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(app.parent/'zephcore/adapters/bench/vendor'),'-I'+str(out),'-I'+str(proto/'nanopb'),str(app/'tests/channel_key_cases.cpp'),'-o',str(out/'channel-key-tests')]
subprocess.run(cmd,check=True)
r=subprocess.run([str(out/'channel-key-tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'tests.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
assert all(hashlib.sha256(Path(n).read_bytes()).hexdigest()==pin for n,pin in pins.items())
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','groups':len([s for s in r.stdout.splitlines()if s.startswith('PASS ')]),'ASAN_UBSAN':True,'short_enums':a.short_enums,'source_pins':pins,'command':cmd,'stock_revision':expected['stock_revision'],'hardware':False,'scope':'Literal stock getKey/generateHash on synthetic primary slots; normalizer/hash/output guards. No secondary lookup integration, plaintext primary, nonce durability or RF proof.'},indent=2)+'\n')
