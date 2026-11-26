#!/usr/bin/env python3
"""Actual volatile peer cache/journal, default and short enums; no hardware."""
if not __debug__:
    raise RuntimeError("Optimized test driver refused")
from pathlib import Path
import argparse,datetime,hashlib,json,os,re,shlex,shutil,signal,sys

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
SOURCE=ROOT/'mt_only/src'
VENDOR=ROOT/'zephcore/adapters/bench/vendor'
sys.path.insert(0,str(HERE.parent/'airtime_quality'))
from bounded_process import run_group

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def require(ok,message):
    if not ok:raise RuntimeError(message)
def interrupted(signum,frame):raise InterruptedError(signum)
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--compiler',default=os.environ.get('CXX','c++'))
    args=parser.parse_args();out=args.output.resolve()
    require(not out.is_relative_to(ROOT),'Output must be outside source worktree')
    require((SOURCE/'PeerLastHeard.h').is_file(),'Actual qualified helper required; no substitute')
    compiler=shlex.split(args.compiler);require(bool(compiler),'Missing compiler')
    compiler[0]=str(Path(shutil.which(compiler[0])).resolve(strict=True))
    paths=[SOURCE/name for name in ('NodeCache.h','NodeJournal.h','SettingsJournal.h','Config.h','PeerLastHeard.h','PeerDeviceMetrics.h','PeerPosition.h','PeerStatus.h','PrimaryChannel.hpp')]
    paths += [VENDOR/name for name in ('MtNodeInfo.h','MtCodec.h','MtKeyCommand.h','RelayCore.h')]
    paths += [HERE/'cache_cases.cpp',Path(__file__).resolve(),HERE.parent/'airtime_quality/bounded_process.py',Path(compiler[0])]
    pins={str(path):sha(path)for path in paths}
    out.mkdir(parents=True,exist_ok=False);(out/'tmp').mkdir()
    summary=dict(status='RUNNING',hardware=False,RF=False,ASAN_UBSAN=True,inputs_sha256=pins,variants=[])
    (out/'INPUTS.json').write_text(json.dumps(pins,indent=2)+'\n')
    env=dict(os.environ,TMPDIR=str(out/'tmp'),ASAN_OPTIONS='halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1',LC_ALL='C')
    def verify():require(all(sha(Path(path))==pin for path,pin in pins.items()),'Test input changed')
    def run(argv,label,timeout):
        path=out/(label+'.log')
        with path.open('x')as stream:result=run_group(argv,timeout=timeout,stdout=stream,env=env,cwd=HERE)
        return dict(argv=argv,exit=result.returncode,log_sha256=sha(path),owned_process_cleanup={'leader_reaped':True,'group_gone':True},output=path.read_text())
    try:
        for name,flags in [('default',[]),('short-enums',['-fshort-enums'])]:
            verify();binary=out/name
            compiled=run([*compiler,'-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*flags,'-I',str(SOURCE),'-I',str(VENDOR),str(HERE/'cache_cases.cpp'),'-o',str(binary)],name+'-compile',60)
            verify();tested=run([str(binary)],name+'-test',15)
            terminal=re.findall(r'^SUMMARY groups=(\d+) checks=(\d+) failures=(\d+)$',tested['output'],re.M)
            sizes=re.findall(r'^SIZES entry=(\d+) cache=(\d+) journal=(\d+) wire_entry=(\d+) wire_record=(\d+)$',tested['output'],re.M)
            require(len(terminal)==len(sizes)==1,'Missing unique fixture result')
            groups,checks,failures=map(int,terminal[0]);require(groups==10 and checks>200 and failures==0,'Fixture result mismatch')
            summary['variants'].append(dict(name=name,compile=compiled,test=tested,groups=groups,checks=checks,failures=failures,sizes=dict(zip(('entry','cache','journal','wire_entry','wire_record'),map(int,sizes[0]))),binary_sha256=sha(binary)))
        verify();summary.update(status='PASS',source_unchanged=True)
    except BaseException as error:
        summary.update(status='FAIL',error_type=type(error).__name__,owned_process_cleanup=getattr(error,'owned_process_cleanup',None));raise
    finally:
        summary['finished_at']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        summary['input_pins_match_final_readback']=all(sha(Path(path))==pin for path,pin in pins.items())
        (out/'SUMMARY.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({key:summary[key]for key in ('status','source_unchanged','input_pins_match_final_readback')}))

if __name__=='__main__':
    for number in (signal.SIGTERM,signal.SIGHUP):signal.signal(number,interrupted)
    main()
