#!/usr/bin/env python3
"""Exact native observation A/B plus real idle-RX hook under synthetic HAL/RTOS."""
if not __debug__:raise RuntimeError('Optimized native qualification refused')
import argparse,hashlib,json,os,re,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--zephyr',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
app=Path(__file__).resolve().parents[1];tests=app/'tests';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
z=a.zephyr.resolve()if a.zephyr else app/'dependency-overlay/zephyr'
driver=z/'drivers/lora/native/sx126x/sx126x.c';header=z/'include/zephyr/drivers/lora/sx126x_ext.h';s=driver.read_text();h=header.read_text()
def extract(text,start):
 begin=text.index(start);brace=text.index('{',begin);depth=1;i=brace+1
 while depth:
  depth+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[begin:i]
enum=extract(h,'enum sx126x_rx_busy_reason')+';\n'
invalidate=extract(s,'void sx126x_airtime_observer_reconfigure_invalidate(')
functions='\n\n'.join([invalidate]+[extract(s,n)for n in ('static bool sx126x_channel_stale_uncertain(','static int sx126x_activity_result(','static int sx126x_rx_activity_detail(','static int sx126x_rx_activity(','bool sx126x_is_receiving_diagnostic(')])
prefix=(tests/'native_activity_prefix.c').read_text();baseline=(tests/'native_activity_baseline.c').read_text();cases=(tests/'native_activity_cases.c').read_text()
# Keep the extracted driver's observer invalidation real. This narrow host
# seam models balanced lock ownership and the exact atomic state touched by
# the production helper; it does not model Zephyr scheduling or IRQ masking.
prefix=prefix.replace('typedef int32_t atomic_val_t;\n', '''typedef int32_t atomic_val_t;
struct k_spinlock { bool held; };
typedef unsigned k_spinlock_key_t;
static unsigned observation_locks, observation_unlocks;
static k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
  assert(lock && !lock->held); lock->held = true; ++observation_locks; return 0x5a;
}
static void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
  assert(lock && lock->held && key == 0x5a); lock->held = false; ++observation_unlocks;
}
''')
prefix=prefix.replace('struct sx126x_data {int lock,dio1_wq;struct k_work irq_work;bool rx_packet_active;\n', '''struct sx126x_data {int lock,dio1_wq;struct k_work irq_work;bool rx_packet_active;
    atomic_t observation_enabled,observation_reconfigure_permit,observation_config_state;
    struct k_spinlock observation_lock;
''')
flags=['-Wall','-Wextra','-Werror','-Wno-unused-function','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer'];commands=[]
def run(name,text,cxx=False):
 suffix='.cpp'if cxx else'.c';source=out/(name+suffix);source.write_text(text);exe=out/name
 compiler=shlex.split(os.environ.get('CXX'if cxx else'CC','c++'if cxx else'cc'))
 cmd=[*compiler,'-std=c++17'if cxx else'-std=c11',*flags,'-I'+str(app/'src'),str(source),'-o',str(exe)];commands.append(cmd);subprocess.run(cmd,check=True)
 r=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/(name+'.log')).write_text(r.stdout+r.stderr);r.check_returncode();print(r.stdout,end='');return r.stdout
before=run('original',prefix+'\n'+baseline+'\n'+cases)
after=run('instrumented',prefix+'\n'+enum+'\n'+functions+'\n'+cases);assert before==after,'Passive detail changed native I/O/state/return trace'
detail=run('detail',prefix+'\n'+enum+'\n'+functions+'\n'+(tests/'native_activity_detail_cases.c').read_text())
idle=run('idle',prefix+'\n'+enum+'\n'+functions+'\n'+(tests/'idle_rx_tests.cpp').read_text(),True)
invalidate_cases='''
int main(void) {
  boot(0); atomic_set(&data.observation_enabled, 1);
  atomic_set(&data.observation_reconfigure_permit, 2);
  atomic_set(&data.observation_config_state, 0);
  sx126x_airtime_observer_reconfigure_invalidate(&dev);
  assert(atomic_get(&data.observation_reconfigure_permit) == 0);
  assert(atomic_get(&data.observation_config_state) == 2);
  assert(observation_locks == 1 && observation_unlocks == 1 && !data.observation_lock.held);
  atomic_set(&data.observation_config_state, 1);
  atomic_set(&data.observation_reconfigure_permit, 2);
  sx126x_airtime_observer_reconfigure_invalidate(&dev);
  assert(atomic_get(&data.observation_config_state) == 1);
  assert(atomic_get(&data.observation_reconfigure_permit) == 0);
  assert(observation_locks == 2 && observation_unlocks == 2 && !data.observation_lock.held);
  boot(0); atomic_set(&data.observation_reconfigure_permit, 2);
  sx126x_airtime_observer_reconfigure_invalidate(&dev);
  assert(atomic_get(&data.observation_reconfigure_permit) == 2 && observation_locks == 2);
  puts("PASS extracted observer invalidation: enabled/reset, configured/preserve, disabled/no-lock");
}
'''
invalidation=run('invalidate',prefix+'\n'+enum+'\n'+invalidate+'\n'+invalidate_cases)
main=(app/'src/main.cpp').read_text().split('int main()',1)[1]
assert main.count('mt::observeIdleRx(service,radio);')==1
assert main.index('k_msgq_get(&rxQueue')<main.index('mt::observeIdleRx(service,radio);')<main.index('drainUsbCompanion(false);')
assert main.index('static void drainUsbCompanion(')<main.index('takeUsbBytes(bytes,sizeof(bytes))')<main.index('companion.byte(bytes[i]')
assert 'return sx126x_is_receiving_diagnostic(radioDevice,&busyDiagnostic_);'in(app/'src/main.cpp').read_text()
paths=[driver,header,Path(__file__).resolve(),app/'src/IdleRx.h',app/'src/main.cpp',tests/'native_activity_prefix.c',tests/'native_activity_baseline.c',tests/'native_activity_cases.c',tests/'native_activity_detail_cases.c',tests/'idle_rx_tests.cpp']
(out/'SUMMARY.json').write_text(json.dumps({'status':'PASS','hardware':False,'RF':False,'actual_extracted_driver_bodies':True,'actual_reconfigure_invalidation_helper_extracted':True,'invalidation_stdout':invalidation,'original_instrumented_io_state_return_traces_identical':True,'main_drain_observe_admin_order':True,'detail_stdout':detail,'idle_stdout':idle,'source_sha256':{str(q):hashlib.sha256(q.read_bytes()).hexdigest()for q in paths},'commands':commands},indent=2)+'\n')
