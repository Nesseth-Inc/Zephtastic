#!/usr/bin/env python3
"""Literal pinned stock interval differential plus pure ZT policy; no I/O/RF."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,subprocess,sys
if not __debug__:raise RuntimeError('Optimized interval qualification refused')
sys.dont_write_bytecode=True
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];root=app.parent;out=a.output.resolve();fixture=app/'tests/module_intervals'
if out==root or root in out.parents:p.error('Output must be outside the source tree')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
provenance=json.loads((fixture/'stock/PROVENANCE.json').read_text())
assert provenance['stock_commit']=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
for name,row in provenance['files'].items():
 source=fixture/'stock'/name;assert source.stat().st_size==row['bytes'] and sha(source)==row['sha256']
inputs=[Path(__file__).resolve(),app/'tests/module_intervals_cases.cpp',*(app/'src'/n for n in ('ModuleIntervals.h','NeighborIntervals.h','NeighborReplyWindow.h')),*sorted(p for p in fixture.rglob('*')if p.is_file())]
pins={str(p.relative_to(root)):sha(p)for p in inputs};out.mkdir(parents=True,exist_ok=False);commands=[];profiles=[]
for event in (0,1):
 label='event'if event else'normal';binary=out/(label+'-tests')
 command=[*shlex.split(os.environ.get('CXX','clang++')),'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',*(['-fshort-enums']if a.short_enums else[]),'-DUSERPREFS_EVENT_MODE='+str(event),'-I'+str(app/'src'),'-I'+str(fixture/'stock'),'-I'+str(fixture/'stubs'),str(app/'tests/module_intervals_cases.cpp'),str(fixture/'stock/Default.cpp'),'-o',str(binary)]
 commands.append(command)
 with(out/(label+'.build.log')).open('x')as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
 result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(label+'.log')).write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');result.check_returncode()
 groups=[line for line in result.stdout.splitlines()if line.startswith('PASS ')];assert len(groups)==8
 summary=[line for line in result.stdout.splitlines()if line.startswith('COMPARISONS ')];assert len(summary)==1
 fields=summary[0].split();assert len(fields)==4 and fields[0]=='COMPARISONS'and fields[2]=='GROUPS'and fields[1].isdigit()and fields[3]=='8'
 profiles.append({'event_mode':bool(event),'groups':len(groups),'literal_stock_comparisons':int(fields[1]),'intentional_post_float_clamp_difference_asserted':True})
assert all(sha(root/name)==digest for name,digest in pins.items())
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'short_enums':a.short_enums,'profiles':profiles,'stock_commit':provenance['stock_commit'],'source_pins':pins,'source_unchanged':True,'commands':commands,'hardware':False,'RF':False,'limits':['Resolved modem/role/region inputs use explicit host seams; no stock resolver, online discovery, real region-profile, Service schedule or hardware proof.','Unknown-online refusal and accepted-zero reply window are explicit local contracts.','Post-float INT32 boundary intentionally clamps more conservatively than literal stock.']},indent=2)+'\n')
