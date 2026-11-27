#!/usr/bin/env python3
"""Run bounded native checks for the isolated canned-message settings journal."""
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
APP=ROOT/"mt_only"
VENDOR=ROOT/"zephcore/adapters/bench/vendor"
SOURCE=APP/"tests/canned_message_journal_tests.cpp"
SETTINGS=APP/"src/modules/CannedMessageSettings.h"
JOURNAL=APP/"src/modules/CannedMessageJournal.h"
VENDOR_NODE=ROOT/"zephcore/adapters/bench/vendor/MtNodeInfo.h"

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(command,env):
    print("$"," ".join(map(str,command)),flush=True)
    result=subprocess.run(command,env=env,text=True,capture_output=True)
    if result.stdout: print(result.stdout,end="",flush=True)
    if result.stderr: print(result.stderr,end="",flush=True)
    if result.returncode: raise SystemExit(result.returncode)
    return result.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Save per-ABI scenario counts')
    args = parser.parse_args()
    groups = {}
    pinned=(SOURCE,SETTINGS,JOURNAL,APP/"src/SettingsJournal.h",APP/"src/Config.h",VENDOR_NODE)
    before={str(p):digest(p) for p in pinned}
    env=dict(os.environ);env["ASAN_OPTIONS"]="detect_leaks=0:halt_on_error=1";env["UBSAN_OPTIONS"]="halt_on_error=1"
    cxx=shlex.split(env.get("CXX","c++"))
    with tempfile.TemporaryDirectory(prefix="mt-canned-journal-") as temporary:
        out=Path(temporary)
        for label,extra in (("default-enum",[]),("short-enum",["-fshort-enums"])):
            exe=out/("canned-journal-"+label)
            run([*cxx,"-std=c++17","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
                 "-fno-omit-frame-pointer","-g","-O1",*extra,"-I"+str(APP/"src"),"-I"+str(VENDOR),
                 str(SOURCE),"-o",str(exe)],env)
            output = run([str(exe)],env)
            counts = [int(line.split()[1]) for line in output.splitlines()
                      if line.startswith('CANNED_MESSAGE_JOURNAL_GROUPS ')]
            if len(counts) != 1 or counts[0] <= 0:
                raise ValueError('Missing canned journal scenario count')
            groups[label] = counts[0]
    after={str(p):digest(p) for p in pinned}
    assert before==after,"source changed during native journal tests"
    if args.output:
        args.output.write_text(json.dumps({'groups': groups, 'asan_ubsan': True, 'hardware': False}) + '\n')
    print("PASS canned-message journal default and short-enum ASan/UBSan variants; source pins unchanged")

if __name__=="__main__": main()
