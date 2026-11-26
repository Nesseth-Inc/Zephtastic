#!/usr/bin/env python3
"""Compile the unchanged previous Config/SettingsJournal as a second reader."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[3]
stock='a0844347c45d1eb85745b49beba7634b29e0fab5'
case=Path(__file__).with_name('rollback.cpp')
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)
    for filename in ('Config.h','SettingsJournal.h'):
        (p/filename).write_bytes(subprocess.check_output(['git','-C',str(repo),'show',f'{stock}:mt_only/src/{filename}']))
    for abi in ([],['-fshort-enums']):
        for new in (False,True):
            flags=['-DCONFIG_MT_LOCAL_CLOCK=1'] if new else ['-I'+str(p)]
            subprocess.run([*shlex.split(os.environ.get('CXX','c++')),'-std=c++17','-g','-fsanitize=address,undefined',
                            '-Wall','-Wextra','-Werror',*abi,*flags,
                            '-I'+str(repo/'mt_only/src'),'-I'+str(repo/'zephcore/adapters/bench/vendor'),
                            str(case),'-o',str(p/('writer' if new else 'reader'))],check=True)
        for generation in ('2','5'):
            for exe in ('writer','reader'):
                subprocess.run([p/exe,p/'bank',generation],check=True)
print('PASS both historical V24 banks, selected and separately recovered, before opt-in and after clear; 4 write-cut cases; both enum ABIs')
