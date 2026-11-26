#!/usr/bin/env python3
"""Actual trace passivity plus explicit M62 logged-field availability changes."""
import argparse,hashlib,json,os,re,shlex,sys,time
from pathlib import Path
if not __debug__:raise RuntimeError('Assertions are required')
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent;APP=HERE.parents[2];SRC=APP/'src'
sys.path.insert(0,str(APP/'tests/airtime_quality'))
from bounded_process import run_group,install_termination_handler
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
out=args.output.resolve()
if out==APP.parent or APP.parent in out.parents:parser.error('Output must be outside the source tree')
if not(SRC/'ChannelTrace.h').is_file():parser.error('Actual completed ChannelTrace.h required; no substitute is admitted')
out.mkdir(parents=True,exist_ok=False)
legacy=json.loads((HERE/'LEGACY-PINS.json').read_text())
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
paths=[SRC/'ChannelTrace.h',SRC/'ChannelMeter.h',HERE/'legacy/ChannelMeter.h',HERE/'legacy/ChannelWindow.h',HERE/'diagnostic-baseline/ChannelMeter.h',HERE/'trace_tests.cpp',HERE/'meter_passivity.cpp',HERE/'LEGACY-PINS.json',Path(__file__).resolve(),APP/'tests/airtime_quality/bounded_process.py']
paths += [SRC/name for name in legacy['unchanged_dependencies_sha256']]
pins={str(p):sha(p)for p in paths};(out/'INPUTS.json').write_text(json.dumps(pins,indent=2)+'\n')
compiler=shlex.split(os.environ.get('CXX','c++'))
if not compiler:raise RuntimeError('CXX must name a compiler')
install_termination_handler();commands=[];status='FAIL';started=time.monotonic();source_checks={};modes={}
def run(argv,log,seconds):
    entry={'argv':argv,'timeout_seconds':seconds,'log':str(log.name)};commands.append(entry)
    try:
        with log.open('w')as stream:run_group(argv,timeout=seconds,stdout=stream,cwd=out,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1','PYTHONDONTWRITEBYTECODE':'1'})
    except BaseException as error:
        entry.update(error_type=type(error).__name__,cleanup=getattr(error,'owned_process_cleanup',{}));raise
    entry.update(leader_reaped=True,group_gone=True,returncode=0)
    return log.read_text()
def extract(text,start):
    pos=text.index(start);brace=text.index('{',pos);depth=1;i=brace+1
    while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[pos:i]
def compare_observations(before,after):
    left=[row.split()for row in before.splitlines()];right=[row.split()for row in after.splitlines()]
    assert len(left)==len(right)==1520
    extra=both=changed=0;supported_reads=0
    for old,new in zip(left,right):
        assert len(old)==len(new)==9
        assert old[:2]==new[:2]and old[5:8]==new[5:8],'label/drain/bound/support changed'
        oldlog=''if old[8]=='-'else old[8];newlog=''if new[8]=='-'else new[8]
        if new[6]=='1'and new[7]=='0':
            # The newly available unsupported/startup path intentionally reads
            # one clock sample at the end of percent(), after its drain.
            assert newlog.endswith('C');newlog=newlog[:-1];extra+=1
        else:supported_reads+=new[6]=='1'
        assert int(new[4])==int(old[4])+extra,'Unexpected clock count change'
        assert newlog==oldlog,'Existing operation/drain/clock order changed'
        if old[2]=='1':
            assert new[2]=='1'and old[3]==new[3],'Previously present numeric value changed';both+=1
        if old[2]!=new[2]:changed+=1
    assert both>0 and changed>0 and extra>0 and supported_reads>0
    return dict(rows=len(left),previously_present_values_identical=both,intended_availability_difference_rows=changed,
                intentional_unsupported_clock_samples=extra,supported_read_rows=supported_reads,
                all_drains_and_existing_clock_order_identical=True)
try:
    assert sha(HERE/'legacy/ChannelMeter.h')==legacy['legacy_ChannelMeter_sha256'],'Frozen legacy meter changed'
    assert legacy['intentional_current_dependency_changes']==['ChannelWindow.h']
    assert sha(HERE/'legacy/ChannelWindow.h')==legacy['legacy_ChannelWindow_sha256']
    assert sha(HERE/'diagnostic-baseline/ChannelMeter.h')==legacy['diagnostic_baseline_ChannelMeter_sha256']
    assert sha(SRC/'ChannelTrace.h')==legacy['diagnostic_baseline_ChannelTrace_sha256']
    for name,expected in legacy['unchanged_dependencies_sha256'].items():
        path=HERE/'legacy/ChannelWindow.h'if name=='ChannelWindow.h'else SRC/name
        assert sha(path)==expected,'Unchanged metric dependency drifted: '+name
    oldwindow=(HERE/'legacy/ChannelWindow.h').read_text();newwindow=(SRC/'ChannelWindow.h').read_text()
    for signature in ('bool record(','void invalidate(','Observed<uint64_t> windowSumMs(','Observed<float> percentAt(','bool anchored('):
        assert extract(oldwindow,signature)==extract(newwindow,signature),'Existing quality/window API changed'
    oldmeter=(HERE/'diagnostic-baseline/ChannelMeter.h').read_text();newmeter=(SRC/'ChannelMeter.h').read_text()
    assert oldmeter.replace(extract(oldmeter,'zt::Observed<float> percent()'),'')==newmeter.replace(extract(newmeter,'zt::Observed<float> percent()'),''),'Meter changes outside intended percent() policy'
    header=(SRC/'ChannelTrace.h').read_text()
    assert len(header.splitlines())<=100
    assert re.findall(r'^\s*#\s*include\s*[<"]([^>"\n]+)',header,re.M)==['cstdint']
    source_checks={'legacy_meter_pin_verified':True,'frozen_legacy_window_pin_verified':True,'unchanged_metric_dependencies_verified':True,'existing_window_quality_API_bodies_unchanged':True,'diagnostic_meter_unchanged_outside_percent':True,'trace_only_cstdint_include':True,'trace_lines':len(header.splitlines())}
    for short in (False,True):
        mode='short'if short else'default';folder=out/mode;folder.mkdir();outputs={}
        flags=['-std=c++17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer']+(['-fshort-enums']if short else[])
        for name in ('trace','legacy-meter','diagnostic-baseline','current-meter'):
            fixture=HERE/('trace_tests.cpp'if name=='trace'else'meter_passivity.cpp');exe=folder/name
            include=['-I'+str(SRC)]
            if name=='legacy-meter':include=['-I'+str(HERE/'legacy'),*include]
            if name=='diagnostic-baseline':include=['-I'+str(HERE/'diagnostic-baseline'),'-I'+str(HERE/'legacy'),*include]
            defines=['-DCHANNEL_DIAGNOSTICS=1']if name in('current-meter','diagnostic-baseline')else[]
            run([*compiler,*flags,*defines,*include,str(fixture),'-o',str(exe)],folder/(name+'.build.log'),45)
            outputs[name]=run([str(exe)],folder/(name+'.log'),10)
        comparison=compare_observations(outputs['legacy-meter'],outputs['current-meter'])
        counters_before=run([str(folder/'diagnostic-baseline'),'--counter-trace'],folder/'baseline-counters.log',10)
        counters_after=run([str(folder/'current-meter'),'--counter-trace'],folder/'current-counters.log',10)
        assert counters_before==counters_after,'Trace counters/metadata/bound/support/pending/drain changed on initialized path'
        detail=run([str(folder/'current-meter'),'--diagnostics-only'],folder/'diagnostics.log',10)
        matrix=run([str(folder/'current-meter'),'--availability-matrix'],folder/'availability-matrix.log',10)
        assert len(matrix.splitlines())==6
        modes[mode]={'status':'PASS','trace_contract_groups':len(outputs['trace'].splitlines()),'observable_comparison':comparison,'diagnostic_counter_trace_rows':len(counters_after.splitlines()),'diagnostic_counter_trace_identical':True,'availability_matrix_cases':6,'diagnostic_detail':detail,'availability_matrix':matrix}
    status='PASS'
except BaseException as error:
    failure=type(error).__name__+': '+str(error);print(failure,file=sys.stderr)
finally:
    after={}
    for p in paths:
        try:after[str(p)]=sha(p)
        except OSError as error:after[str(p)]='unreadable:'+type(error).__name__
    changed=[p for p in pins if pins[p]!=after[p]]
    receipt={'status':status if not changed else'STALE','error':locals().get('failure'),'input_sha256':pins,'input_sha256_after':after,'changed':changed,'legacy_revision':legacy['source_revision'],'source_checks':source_checks,'modes':modes,'commands':commands,'hardware':False,'network':False,'radio':False,'elapsed_seconds':time.monotonic()-started}
    (out/'SUMMARY.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({'status':receipt['status'],'error':receipt['error'],'changed':changed,'modes':modes}))
sys.exit(0 if status=='PASS'and not changed else 1)
