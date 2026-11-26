#!/usr/bin/env python3
"""Exercise settings migration/journal failures under ASAN/UBSAN; no hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(app.parent))
import host_test_toolchain as host
inputs=[Path(__file__).resolve(),app/'tests/persistence_tests.cpp',app/'src/Config.h',app/'src/SettingsJournal.h']
inputs.extend([Path(host.__file__), *(app/'src/stock/position').glob('*.[ch]*')])
pins={str(path):hashlib.sha256(path.read_bytes()).hexdigest()for path in inputs}
command=[*shlex.split(os.environ.get('CXX','g++')),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
         '-I'+str(app/'src'),'-I'+str(app.parent/'zephcore/adapters/bench/vendor'),
         str(app/'tests/persistence_tests.cpp'),*host.position_policy_link(app,out),'-o',str(out/'persistence-tests')]
subprocess.run(command,check=True)
result=subprocess.run([str(out/'persistence-tests')],text=True,capture_output=True,
                      env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr)
result.check_returncode()
metadata=[line for line in result.stdout.splitlines()if line.startswith('PERSISTENCE_METADATA ')]
assert len(metadata)==1
fields=dict(word.split('=',1)for word in metadata[0].split()[1:])
assert set(fields)=={'record_size','partial_write_cut_points_per_path','corruption_single_bits'}
assert all(value.isascii()and value.isdecimal()for value in fields.values())
counts={key:int(value)for key,value in fields.items()}
assert counts['record_size']>0 and counts['partial_write_cut_points_per_path']==counts['record_size']and counts['corruption_single_bits']==counts['record_size']*8
assert all(hashlib.sha256(Path(name).read_bytes()).hexdigest()==digest for name,digest in pins.items())
(out/'results.json').write_text(json.dumps({'PASS':True,'ASAN_UBSAN':True,'hardware':False,
  'simulated_record_size':counts['record_size'],'partial_write_cut_points_per_path':counts['partial_write_cut_points_per_path'],
  'corruption_single_bits':counts['corruption_single_bits'],'source_pins':pins,'source_unchanged':True,'command':command},indent=2)+'\n')
