#!/usr/bin/env python3
"""Pin/extract native observer bodies; bounded synthetic C11 HAL/RTOS tests.

The Apache-2.0 baseline is frozen M60d2, not an installed dependency. No radio,
network or filesystem mutation beyond the explicitly selected output folder.
"""
import argparse,hashlib,json,os,re,shlex,signal,subprocess,sys,time
from pathlib import Path
if not __debug__:raise RuntimeError('Optimized native observer qualification refused')
HERE=Path(__file__).resolve().parent
APP=HERE.parents[2]
sys.dont_write_bytecode=True
sys.path.insert(0,str(APP/'tests/airtime_quality'))
from bounded_process import run_group,install_termination_handler
DRIVER=Path('dependency-overlay/zephyr/drivers/lora/native/sx126x/sx126x.c')
HEADER=DRIVER.with_name('sx126x.h')
EXT=Path('dependency-overlay/zephyr/include/zephyr/drivers/lora/sx126x_ext.h')
MATH=DRIVER.with_name('sx126x_airtime_math.h')
HAL=DRIVER.with_name('sx126x_hal_common.c')
BASELINE=HERE/'baseline-sx126x.c'
BASELINE_SHA='bcf90085138152d2cc3c14339767b56907b24ca150bacf66287de82f2cfe7e6d'
ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);a=ap.parse_args();out=a.output.resolve()
if out==APP.parent or APP.parent in out.parents:ap.error('Output must be outside the source tree')
out.mkdir(parents=True,exist_ok=False)
src=APP
paths=[src/DRIVER,src/HEADER,src/EXT,src/MATH,src/HAL,APP/'native-driver.cmake',APP/'dependency-lock.json',BASELINE,HERE/'prefix.c',HERE/'trace_cases.c',HERE/'observer_cases.c',HERE/'observer_info_cases.c',HERE/'reconfigure_cases.c',HERE/'tx_timeout_cases.c',HERE/'stage_cases.c',HERE/'lifecycle_trace_cases.c',HERE/'lifecycle_observer_cases.c',HERE/'dispatch_trace_cases.c',HERE/'dispatch_observer_cases.c',HERE/'stale_setup.c',HERE/'stale_trace_cases.c',HERE/'stale_observer_cases.c',Path(__file__).resolve(),APP/'tests/airtime_quality/bounded_process.py']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pins={str(p):sha(p)for p in paths};(out/'INPUTS.json').write_text(json.dumps(pins,indent=2)+'\n')
def extract(text,start):
    pos=text.index(start);opening=text.index('{',pos);depth=1;i=opening+1
    while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[pos:i]
s=(src/DRIVER).read_text();baseline=BASELINE.read_text();ext=(src/EXT).read_text()
observer_names=['static uint32_t sx126x_stage_begin(', 'static void sx126x_stage_fail(', 'void sx126x_airtime_observer_reconfigure_invalidate(','static void sx126x_observe_reconfigure_reserve(','static void sx126x_observe_reconfigure_quiescent(','static void sx126x_observe_tx_started(','static void sx126x_observe_tx_irq(','static void sx126x_observe_loss(','static void sx126x_observe_rx(','static void sx126x_observe_config(','int sx126x_airtime_observer_start(','bool sx126x_airtime_observer_supported(','int sx126x_airtime_observer_info(','int sx126x_airtime_observer_take(','bool sx126x_airtime_observer_take_loss(','static bool sx126x_channel_stale_uncertain(']
terminal_names=['static void sx126x_handle_irq_tx_done(','static int sx126x_handle_irq_rx_done(','static void sx126x_handle_irq_timeout(','static int sx126x_handle_irq_status(','static void sx126x_irq_work_handler(']
lifecycle_names=['static int sx126x_restart_rx(','static int sx126x_lora_recv_async(','static int sx126x_lora_config(']
pause_names=['static void sx126x_storage_standby_state(','static int sx126x_storage_settle_after_standby(','int sx126x_pause_for_storage(']
lifecycle_current=['static int sx126x_restart_rx(','static int sx126x_recv_async_common(','static int sx126x_lora_recv_async(','static int sx126x_lora_config(']
dispatch_names=['static void sx126x_dio1_callback(','static int sx126x_activity_result(','static int sx126x_rx_activity_detail(']
observ='\n\n'.join(extract(s,n)for n in observer_names)
prefix=(HERE/'prefix.c').read_text().replace('/* STAGE_INFO_STRUCT */',extract(ext,'struct sx126x_stage_info {')+';').replace('/* OBSERVATION_STRUCT */',extract(ext,'struct sx126x_airtime_observation {')+';')
packet_typedef=ext[ext.index('typedef void (*sx126x_recv_packet_cb)'):]
packet_typedef=packet_typedef[:packet_typedef.index(';')+1]
prefix=prefix.replace('/* PACKET_STATUS_DECLS */',extract(ext,'struct sx126x_packet_status {')+';\n'+packet_typedef)
prefix+='\n'+extract(ext,'struct sx126x_airtime_observer_info {')+';\n'
prefix+='\n'+extract(ext,'enum sx126x_rx_busy_reason')+';\n'
math=(src/MATH).read_text();match=re.search(r'(?:static\s+inline\s+)?struct sx126x_airtime_value\s+sx126x_airtime_estimate\s*\(',math);assert match,'actual math function not found';math=extract(math,match.group(0))
prefix=prefix.replace('/* AIRTIME_FUNCTION */',math)
commands=[];processes=[];logs={};negative_controls={};source_checks={};status='FAIL';started=time.time()
compiler=shlex.split(os.environ.get('CC','cc'))
if not compiler:raise RuntimeError('CC must name a compiler or compiler wrapper')
install_termination_handler()
def owned(argv,timeout,path,env=None,cwd=None):
    record={'argv':argv,'timeout_seconds':timeout,'cwd':str(cwd)if cwd else None};processes.append(record)
    try:
        with path.open('w')as stream:run_group(argv,timeout=timeout,stdout=stream,env=env,cwd=cwd)
        record.update(leader_reaped=True,group_gone=True,status='PASS')
    except BaseException as error:
        record.update(status='FAIL',error_type=type(error).__name__,cleanup=getattr(error,'owned_process_cleanup',{}));raise
try:
    assert pins[str(BASELINE)]==BASELINE_SHA,'Frozen M60d2 baseline pin mismatch'
    for name in ['int sx126x_bench_abort(','int sx126x_pause_for_storage(']:
        before=extract(baseline,name);after=extract(s,name)
        after=re.sub(r'sx126x_observe_rx\(data,\s*0,\s*0,\s*false\);','',after)
        if name=='int sx126x_pause_for_storage(':
            assert after.count('sx126x_observe_reconfigure_quiescent(data);')==1
            after=after.replace('sx126x_observe_reconfigure_quiescent(data);','')
        before=re.sub(r'/\*.*?\*/','',before,flags=re.S)
        after=re.sub(r'/\*.*?\*/','',after,flags=re.S)
        same=re.sub(r'\s+','',before)==re.sub(r'\s+','',after)
        assert same,f'{name}: changes beyond metadata-only observer call'
        source_checks[name]={'HAL_RTOScode_unchanged_after_removing_exact_metadata_calls':True,'removed_calls':['sx126x_observe_rx(data,0,0,false)']+(['sx126x_observe_reconfigure_quiescent(data)']if name=='int sx126x_pause_for_storage('else[]),'not_target_storage_fault_injection':True}
    rx=extract(s,'static void sx126x_observe_rx(')
    calls=set(re.findall(r'\b([a-zA-Z_]\w*)\s*\(',re.sub(r'/\*.*?\*/','',rx,flags=re.S)))
    calls-={'sx126x_observe_rx','if','return','uint64_t'}
    assert calls <= {'atomic_get','atomic_set','k_uptime_get','sx126x_airtime_estimate','k_msgq_put','k_spin_lock','k_spin_unlock'},calls
    assert rx.index('k_spin_unlock(')<rx.index('sx126x_airtime_estimate(')<rx.index('k_msgq_put(')
    assert 'sx126x_airtime_estimate(&captured, length)' in rx
    source_checks['observer_nonblocking_call_graph']={'direct_calls':sorted(calls),'no_application_callback_or_Service_lock':True,'k_msgq_K_NO_WAIT':True,'estimator_and_queue_after_spin_unlock':True}
    loss=extract(s,'static void sx126x_observe_loss(')
    calls=set(re.findall(r'\b([a-zA-Z_]\w*)\s*\(',loss))-{'sx126x_observe_loss','if'}
    assert calls=={'atomic_get','atomic_set'},calls
    source_checks['ISR_loss_helper_atomic_only']={'direct_calls':sorted(calls),'no_queue_clock_IO_or_application_call':True}
    info=extract(s,'int sx126x_airtime_observer_info(')
    calls=set(re.findall(r'\b([a-zA-Z_]\w*)\s*\(',re.sub(r'/\*.*?\*/','',info,flags=re.S)))-{'sx126x_airtime_observer_info','if'}
    assert calls=={'atomic_get','k_spin_lock','k_spin_unlock'},calls
    source_checks['info_getter_passive_call_graph']={'direct_calls':sorted(calls),'metadata_spin_only_no_HAL_GPIO_application_mutex_queue_clock_loss_mutation':True}
    # Exact metadata-only additions may not hide HAL/RTOS changes above.
    metadata_helpers=['static uint32_t sx126x_stage_begin(', 'static void sx126x_stage_fail(',
        'void sx126x_airtime_observer_reconfigure_invalidate(',
        'static void sx126x_observe_reconfigure_reserve(', 'static void sx126x_observe_reconfigure_quiescent(']
    for helper in metadata_helpers:
        body=extract(s,helper)
        calls=set(re.findall(r'\b([a-zA-Z_]\w*)\s*\(',re.sub(r'/\*.*?\*/','',body,flags=re.S)))
        calls-={helper.split('(')[0].split()[-1],'if'}
        assert calls<={'atomic_get','atomic_set','k_spin_lock','k_spin_unlock'},calls
    config=extract(s,'static int sx126x_lora_config(')
    assert config.index('k_mutex_lock(')<config.index('sx126x_observe_reconfigure_reserve(data);')<config.index('ret = sx126x_ensure_ready(')
    assert s.count('sx126x_observe_reconfigure_quiescent(data);')==1
    for admission in ['static void sx126x_dio1_callback(','static void sx126x_irq_work_handler(']:
        assert extract(s,admission).count('sx126x_airtime_observer_reconfigure_invalidate(dev);')==1
    activity=extract(s,'static int sx126x_rx_activity_detail(')
    assert activity.index('sx126x_airtime_observer_reconfigure_invalidate(dev);')<activity.index('k_work_submit_to_queue(')
    hal=(src/HAL).read_text()
    rf_guard='''if (opcode == SX126X_CMD_SET_RX || opcode == SX126X_CMD_SET_RX_DUTY_CYCLE ||
            opcode == SX126X_CMD_SET_TX || opcode == SX126X_CMD_SET_CAD ||
            opcode == SX126X_CMD_SET_TX_CONTINUOUS_WAVE || opcode == 0xd2)
            sx126x_airtime_observer_reconfigure_invalidate(dev);'''
    assert re.sub(r'\s+','',rf_guard) in re.sub(r'\s+','',hal)
    assert hal.index('sx126x_airtime_observer_reconfigure_invalidate(dev);')<hal.index('ret = spi_transfer(',hal.index('sx126x_airtime_observer_reconfigure_invalidate(dev);'))
    source_checks['reconfigure_permit_wiring']={'metadata_helpers_only':True,'config_reserves_under_application_lock_before_ensure_ready':True,'only_pause_success_mints':True,'both_work_admission_and_worker_entry_invalidate':True,'literal_RF_opcode_chokepoint_checked_and_extracted_for_host_execution':True,'native_HAL_RTOS_trace_comparisons_retained':True}
    for helper in ('static void sx126x_observe_tx_started(','static void sx126x_observe_tx_irq('):
        body=extract(s,helper)
        calls=set(re.findall(r'\b([a-zA-Z_]\w*)\s*\(',re.sub(r'/\*.*?\*/','',body,flags=re.S)))
        calls-={helper.split('(')[0].split()[-1],'if'}
        assert calls<={'atomic_get','k_spin_lock','k_spin_unlock'},calls
    irq=extract(s,'static int sx126x_handle_irq_status(')
    assert irq.index('sx126x_observe_tx_irq(data, irq_status);')<irq.index('sx126x_clear_irq_status(')
    settx=extract(s,'static int sx126x_set_tx(')
    assert settx.index('sx126x_hal_write_cmd(')<settx.index('if (ret == 0) sx126x_observe_tx_started(dev->data);')
    init=extract(s,'static int sx126x_init(')
    assert 'data->observation_tx_started = data->observation_tx_done = 0;'in init
    assert 'data->observation_tx_timeouts = data->observation_last_tx_irq = 0;'in init
    assert 'observation_tx_'not in extract(s,'int sx126x_airtime_observer_start(')
    gate=(APP/'native-driver.cmake').read_text()
    gate_rows=re.findall(r'file\(SHA256 "\$ENV\{ZEPHYR_BASE\}/([^"\n]+)" mt_driver_sha\).*?if\(NOT mt_driver_sha STREQUAL "([a-f0-9]{64})"\)',gate,re.S)
    assert len(gate_rows)==7
    declared={r['path']+'/'+o['path']:o['sha256']for r in json.loads((APP/'dependency-lock.json').read_bytes())for o in r['overlay']}
    for name,digest in gate_rows:assert sha(APP/'dependency-overlay/zephyr'/name)==digest==declared['zephyr/'+name]
    source_checks['passive_TX_facts']={'spin_only_helpers':True,'IRQ_snapshot_before_clear_or_terminal_state_change':True,'SetTx_counter_after_successful_actual_HAL_command':True,'boot_init_only_no_observer_reset':True,'saturating_counters_no_delivery_claim':True,'all7_native_gate_lock_rows_match_actual_bodies':True}
    classifier=extract(s,'static bool sx126x_channel_stale_uncertain(')
    calls=set(re.findall(r'\b([a-zA-Z_]\w*)\s*\(',classifier))-{'sx126x_channel_stale_uncertain','if','uint16_t'}
    assert not calls,calls
    source_checks['stale_classifier_pure']={'direct_calls':[],'no_HAL_GPIO_lock_queue_clock_or_observer_calls':True}
    stage_names=['static int sx126x_restart_rx(', 'static int sx126x_lora_config(',
        'static int sx126x_set_tx(', 'static uint32_t sx126x_lora_airtime(const struct device *dev, uint32_t data_len)\n{',
        'static int sx126x_lora_send_async(']
    stage_body=observ+'\n\n'+'\n\n'.join(extract(s,n)for n in stage_names)
    assert stage_body.count(extract(s,'static int sx126x_lora_send_async('))==1
    stage_no_record=stage_body.replace('slot->stage = stage;', 'slot->stage = 0U * stage;')
    stage_overwrite=stage_body.replace('if (*recorded || result >= 0) return;', 'if (result >= 0) return;')
    assert stage_no_record!=stage_body and stage_overwrite!=stage_body
    source_checks['passive_stage_extraction']={'actual_send_config_restart_SetTx_airtime_and_getter_bodies':True,
        'explicit_all5_metadata_helpers_call_whitelist':metadata_helpers,
        'scope':'synthetic HAL/RTOS including CAD result and post-CAD state injection, not native CAD/SPI execution or target race proof',
        'new_stage_fixture_assertion_groups_each_mode':8,'new_stage_negative_mutants_each_mode':2}
    for short in [False,True]:
        suffix='short'if short else'default';outputs={}
        fixtures=[('baseline','\n\n'.join(extract(baseline,n)for n in terminal_names),'trace_cases.c'),('candidate',observ+'\n\n'+'\n\n'.join(extract(s,n)for n in terminal_names),'trace_cases.c'),('observer',observ+'\n\n'+'\n\n'.join(extract(s,n)for n in terminal_names),'observer_cases.c'),('lifecycle-baseline','\n\n'.join(extract(baseline,n)for n in lifecycle_names),'lifecycle_trace_cases.c'),('lifecycle-candidate',observ+'\n\n'+'\n\n'.join(extract(s,n)for n in lifecycle_current),'lifecycle_trace_cases.c'),('lifecycle-observer',observ+'\n\n'+'\n\n'.join(extract(s,n)for n in lifecycle_current),'lifecycle_observer_cases.c'),('dispatch-baseline',observ+'\n\n'+'\n\n'.join(extract(baseline,n)for n in dispatch_names),'dispatch_trace_cases.c'),('dispatch-candidate',observ+'\n\n'+'\n\n'.join(extract(s,n)for n in dispatch_names),'dispatch_trace_cases.c'),('dispatch-observer',observ+'\n\n'+'\n\n'.join(extract(s,n)for n in dispatch_names),'dispatch_observer_cases.c')]
        reconfigure=observ+'\n\n'+extract(hal,'int sx126x_hal_write_cmd(')+'\n\n'+'\n\n'.join(extract(s,n)for n in lifecycle_current+terminal_names+dispatch_names+pause_names)
        fixtures.append(('reconfigure',reconfigure,'reconfigure_cases.c'))
        txbody=observ+'\n\n'+'\n\n'.join(extract(s,n)for n in terminal_names)+ '\n'+extract(s,'static int sx126x_set_tx(')+'\n'+extract(s,'static uint32_t sx126x_lora_airtime(const struct device *dev, uint32_t data_len)\n{')
        fixtures.append(('tx-timeout',txbody,'tx_timeout_cases.c'))
        current_timeout=extract(s,'static void sx126x_handle_irq_timeout(')
        original_timeout=extract(baseline,'static void sx126x_handle_irq_timeout(')
        assert current_timeout!=original_timeout and txbody.count(current_timeout)==1
        fixtures.append(('tx-timeout-original-branch',txbody.replace(current_timeout,original_timeout),'tx_timeout_cases.c'))
        fixtures.extend([('stages',stage_body,'stage_cases.c'),
            ('stages-no-record',stage_no_record,'stage_cases.c'),
            ('stages-overwrite-first',stage_overwrite,'stage_cases.c')])

        no_mint=reconfigure.replace('sx126x_observe_reconfigure_quiescent(data);','(void)data;')
        assert no_mint!=reconfigure
        fixtures.append(('reconfigure-no-pause-proof',no_mint,'reconfigure_cases.c'))
        bypass=reconfigure.replace('atomic_get(&data->observation_reconfigure_permit) == 2','true')
        assert bypass!=reconfigure
        fixtures.append(('reconfigure-unproved-commit',bypass,'reconfigure_cases.c'))
        info_helpers='\n'.join(extract((HERE/'observer_cases.c').read_text(),n)for n in ('static void enable(','static unsigned drain('))
        fixtures.append(('observer-info',observ+'\n'+info_helpers,'observer_info_cases.c'))
        stale_helpers=extract((HERE/'observer_cases.c').read_text(),'static void enable(')+'\n'+(HERE/'stale_setup.c').read_text()
        fixtures.extend([
            ('stale-baseline',observ+'\n'+'\n\n'.join(extract(baseline,n)for n in dispatch_names)+'\n'+stale_helpers,'stale_trace_cases.c'),
            ('stale-candidate',observ+'\n'+'\n\n'.join(extract(s,n)for n in dispatch_names)+'\n'+stale_helpers,'stale_trace_cases.c'),
            ('stale-observer',observ+'\n'+'\n\n'.join(extract(s,n)for n in dispatch_names)+'\n'+stale_helpers,'stale_observer_cases.c')])
        old_activity,replacements=re.subn(r'sx126x_channel_stale_uncertain\(\s*stale_mask\s*,\s*latest\s*\)','true',extract(s,'static int sx126x_rx_activity_detail('))
        assert replacements==1,'Actual stale classifier integration missing/ambiguous'
        old_dispatch='\n\n'.join(old_activity if n=='static int sx126x_rx_activity_detail('else extract(s,n)for n in dispatch_names)
        fixtures.append(('stale-old-always-taint',observ+'\n'+old_dispatch+'\n'+stale_helpers,'stale_observer_cases.c'))
        for variant,target in [('dispatch-no-dio-loss','static void sx126x_dio1_callback('),('dispatch-no-activity-loss','static int sx126x_rx_activity_detail(')]:
            bodies=[]
            for n in dispatch_names:
                body=extract(s,n)
                if n==target:
                    assert body.count('sx126x_observe_loss(data);')==1
                    body=body.replace('sx126x_observe_loss(data);','(void)data;')
                bodies.append(body)
            fixtures.append((variant,observ+'\n\n'+'\n\n'.join(bodies),'dispatch_observer_cases.c'))
        for name,body,cases in fixtures:
            pre=prefix
            if name.startswith('lifecycle-')or name.startswith('reconfigure')or name.startswith('stages'):pre=re.sub(r'^static int sx126x_restart_rx\([^\n]*\n','',pre,flags=re.M)
            if name.startswith('reconfigure'):pre=re.sub(r'^static int sx126x_hal_write_cmd\([^\n]*\n','',pre,flags=re.M)
            if name.startswith('stages'):
                pre='''/* Stage fixture policy/standalone switches only; all older variants keep IS_ENABLED=0. */
#define CONFIG_LORA_SX126X_NATIVE_STANDALONE 1
#define IS_ENABLED(x) STAGE_CFG_##x
#define STAGE_CFG_CONFIG_MT_ONLY policy_mt
#define STAGE_CFG_CONFIG_MT_ALLOW_TX policy_tx
#define STAGE_CFG_CONFIG_ZEPHCORE_DUAL_RX_BENCH policy_bench
'''+pre
            generated=out/f'{name}-{suffix}.c';generated.write_text(pre+'\n'+body+'\n'+(HERE/cases).read_text());exe=generated.with_suffix('')
            cmd=[*compiler,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',*(['-fshort-enums']if short else[]),str(generated),'-o',str(exe)];commands.append(cmd);owned(cmd,30,out/f'{name}-{suffix}.build.log')
            logfile=out/f'{name}-{suffix}.log'
            if name.startswith('dispatch-no-')or name=='stale-old-always-taint'or name in {'reconfigure-no-pause-proof','reconfigure-unproved-commit','tx-timeout-original-branch','stages-no-record','stages-overwrite-first'}:
                try:owned([str(exe)],15,logfile,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'},cwd=out)
                except subprocess.CalledProcessError as rejected:
                    assert rejected.returncode==-signal.SIGABRT,rejected.returncode
                    expected_assertion=('info.send_stage.stage==stage'if name=='stages-no-record'else'info.restart_stage.stage==2'if name=='stages-overwrite-first'else'signal_calls==1 && signal_result==-ETIMEDOUT'if name=='tx-timeout-original-branch'else'n==expected_events'if name=='stale-old-always-taint'else'(info.config_state==1)==supported'if name.startswith('reconfigure-')else'sx126x_airtime_observer_take_loss')
                    assert expected_assertion in logfile.read_text()
                    assert processes[-1]['cleanup']=={'leader_reaped':True,'group_gone':True},processes[-1]
                    processes[-1]['status']='EXPECTED_REJECTION'
                    negative_controls[name+'-'+suffix]={'status':'PASS','mutant_rejected_at_expected_assertion':expected_assertion,'returncode':rejected.returncode}
                else:raise AssertionError('Native observer negative control was not rejected')
            else:owned([str(exe)],15,logfile,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'},cwd=out)
            outputs[name]=logfile.read_text()
        assert outputs['baseline']==outputs['candidate'],'disabled observer changed HAL/callback/state/return traces'
        assert outputs['lifecycle-baseline']==outputs['lifecycle-candidate'],'disabled observer changed lifecycle HAL/state/return traces'
        assert outputs['dispatch-baseline']==outputs['dispatch-candidate'],'observer changed dispatch HAL/gates/results'
        assert outputs['stale-baseline']==outputs['stale-candidate'],'stale filter changed existing HAL/busy/rechecks/stamps/results'
        logs[suffix]=outputs['observer']+outputs['lifecycle-observer']+outputs['dispatch-observer']+outputs['observer-info']+outputs['stale-observer']+outputs['reconfigure']+outputs['tx-timeout']+outputs['stages']
    status='PASS'
except BaseException as e:
    error=f'{type(e).__name__}: {e}';print(error,file=sys.stderr)
finally:
    after={}
    for p in paths:
        try:after[str(p)]=sha(p)
        except OSError as failure:after[str(p)]='unreadable:'+type(failure).__name__
    changed=[p for p in pins if pins[p]!=after[p]]
    receipt={'status':status if not changed else'STALE','error':locals().get('error'),'source_changed_during_test':changed,'input_sha256':pins,'input_sha256_after':after,'baseline_expected_sha256':BASELINE_SHA,'actual_bodies_extracted':observer_names+terminal_names+lifecycle_current+dispatch_names+pause_names+['static int sx126x_set_tx(','static uint32_t sx126x_lora_airtime(definition)','HAL:int sx126x_hal_write_cmd('],'actual_math_header':True,'reconfigure_positive_groups_each_mode':6,'TX_timeout_positive_groups_each_mode':6,'TX_timeout_original_branch_negative_each_mode':1,'TX_timeout_scope':'actual timeout/IRQ/SetTx/airtime/getter bodies; synthetic HAL/time/signal, no target IRQ race or physical trigger proof','reconfigure_negative_mutants_each_mode':2,'reconfigure_barrier':'actual pause/config/IRQ functions with explicit synthetic callback/work/spin stubs; no target race proof','hardware':False,'RF':False,'HAL_RTOS':'synthetic host stubs, pthread queue; no Zephyr scheduler or actual modem proof','disabled_AB_cases_each_mode':142,'enabled_dispatch_AB_cases_each_mode':6,'total_AB_cases_each_mode':148,'additional_stale_AB_cases_each_mode':80,'stale_activity_queue_cases_each_mode':80,'actual_stale_classifier_checks_each_mode':1114112,'classifier_full_32bit_pair_Cartesian_claim':False,'source_checks':source_checks,'logs':logs,'commands':commands,'negative_controls':negative_controls,'owned_processes':processes,'elapsed_seconds':time.time()-started}
    receipt['actual_bodies_extracted']+=['static int sx126x_lora_send_async(']
    receipt['stage_positive_groups_each_mode']=8
    receipt['stage_negative_mutants_each_mode']=2
    receipt['stage_scope']='Actual send/config/restart/SetTx/airtime/metadata getter bodies with explicit synthetic policy, CAD, state, HAL and RTOS outcomes; no native CAD/SPI, target IRQ/preemption race or physical proof.'
    (out/'SUMMARY.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({k:receipt[k]for k in ['status','error','actual_math_header','source_changed_during_test','elapsed_seconds']}))
sys.exit(0 if status=='PASS'and not changed else 1)
