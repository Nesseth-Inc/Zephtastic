#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Actual notification GPIO driver + literal donor C gate, synthetic Zephyr only.

No device tree compiler, GPIO, radio, threads, firmware build or network action.
Synthetic atomics are deliberately single-thread scalars, not a concurrency test.
"""
if not __debug__:raise RuntimeError('Optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess
app=Path(__file__).resolve().parents[1];root=app.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve()
if out.exists()or out.is_relative_to(root):p.error('fresh output outside source required')
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';helper=root/'zephcore/helpers'
driver=app/'src/notifications/ZephyrNotification.cpp';fixture=app/'tests/notification_gpio_tests.cpp';donor=helper/'led_gate.c'
sha=lambda raw:hashlib.sha256(raw).hexdigest()
t096=root/'zephcore/boards/nrf52840/heltec_t096/heltec_t096_nrf52840.dts';headless=app/'boards/xiao.overlay'
assert 'led0 = &led_white;' in t096.read_text() and 'gpios = <&gpio0 28 GPIO_ACTIVE_HIGH>;' in t096.read_text()
assert '/delete-property/ led0;' in headless.read_text()and'/delete-node/ &led0;' in headless.read_text()
out.mkdir(parents=True);(out/'meshtastic').mkdir();stubs=out/'synthetic';stubs.mkdir()
for f in proto.glob('*.pb.h'):shutil.copyfile(f,out/'meshtastic'/f.name)
headers={
'zephyr/device.h':'''#pragma once
#include <stdint.h>
#include <stdbool.h>
struct device { int synthetic_marker; };
#ifdef __cplusplus
extern "C" {
#endif
extern struct device fixture_gpio_device;
#ifdef __cplusplus
}
#endif
''',
'zephyr/devicetree.h':'''#pragma once
#ifndef SYNTHETIC_LED_PIN
#define SYNTHETIC_LED_PIN 28
#endif
#ifndef SYNTHETIC_LED_FLAGS
#define SYNTHETIC_LED_FLAGS 0
#endif
#ifndef SYNTHETIC_LED_STATUS
#define SYNTHETIC_LED_STATUS 1
#endif
#define DT_ALIAS(name) 1
#define DT_NODE_HAS_STATUS(node,status) SYNTHETIC_LED_STATUS
#define DT_GPIO_PIN(node,property) SYNTHETIC_LED_PIN
#define DT_GPIO_FLAGS(node,property) SYNTHETIC_LED_FLAGS
#define BUILD_ASSERT(condition) static_assert(condition,"synthetic BUILD_ASSERT of actual driver condition")
''',
'zephyr/drivers/gpio.h':'''#pragma once
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#define GPIO_ACTIVE_HIGH 0
#define GPIO_ACTIVE_LOW 1
#define GPIO_OUTPUT_INACTIVE 0x10000u
typedef struct gpio_dt_spec { const struct device *port; uint32_t pin; uint32_t dt_flags; } gpio_dt_spec;
#define GPIO_DT_SPEC_GET(node,property) {&fixture_gpio_device,DT_GPIO_PIN(node,property),DT_GPIO_FLAGS(node,property)}
#ifdef __cplusplus
extern "C" {
#endif
bool fixture_gpio_ready(const gpio_dt_spec *);
int fixture_gpio_configure(const gpio_dt_spec *,uint32_t);
int fixture_gpio_set(const gpio_dt_spec *,int);
#ifdef __cplusplus
}
#endif
static inline bool gpio_is_ready_dt(const gpio_dt_spec *s){return fixture_gpio_ready(s);}
static inline int gpio_pin_configure_dt(const gpio_dt_spec *s,uint32_t flags){return fixture_gpio_configure(s,flags);}
static inline int gpio_pin_set_dt(const gpio_dt_spec *s,int value){return fixture_gpio_set(s,value);}
''',
'zephyr/kernel.h':'''#pragma once
#ifdef __weak
#undef __weak
#endif
#define __weak __attribute__((weak))
#define ARG_UNUSED(value) ((void)(value))
''',
'zephyr/sys/atomic.h':'''#pragma once
/* Single-thread mock: no locking, memory ordering or concurrency claim. */
typedef int atomic_t;
typedef int atomic_val_t;
static inline atomic_val_t atomic_get(const atomic_t *p){return *p;}
static inline atomic_val_t atomic_set(atomic_t *p,atomic_val_t value){atomic_val_t old=*p;*p=value;return old;}
'''}
for name,text in headers.items():path=stubs/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
includes=['-I'+str(x)for x in [stubs,app/'src',helper,vendor,mono,out,proto/'nanopb']]
flags=['-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined']
source_paths={Path(__file__).resolve(),driver,fixture,donor,helper/'led_gate.h',t096,headless}
for compiler,std,src in [(cxx,'c++17',driver),(cxx,'c++17',fixture),(cc,'c11',donor)]:
    command=[*compiler,'-std='+std,*includes,'-MM',str(src)]
    raw=subprocess.run(command,capture_output=True,text=True,check=True,timeout=60).stdout
    for n in shlex.split(raw.replace('\\\n',' ').split(':',1)[1]):
        path=Path(n).resolve()
        if path.parent==out/'meshtastic':
            original=proto/path.name;assert path.read_bytes()==original.read_bytes();path=original
        source_paths.add(path)
pins={str(x):sha(x.read_bytes())for x in source_paths};rows=[];negatives=[]
report={'status':'FAIL','ASAN_UBSAN':True,'hardware_GPIO_or_network':False,'firmware_or_real_Zephyr_SDK_build':False,'synthetic_device_GPIO_DT_and_single_thread_atomics':True,'concurrency_qualification':False,'source_sha256_before':pins,'synthetic_header_sha256':{n:sha(t.encode())for n,t in headers.items()},'positive_executions':rows,'negative_compiles':negatives}
try:
    (out/'compiler.txt').write_text(subprocess.run([*cxx,'--version'],capture_output=True,text=True,check=True,timeout=15).stdout)
    for label,extra in [('default',[]),('short-enums',['-fshort-enums'])]:
        for target,board,groups in [('t096',['-DCONFIG_BOARD_HELTEC_T096=1'],9),('headless',['-DTEST_NOTIFICATION_HEADLESS=1','-DSYNTHETIC_LED_STATUS=0'],3)]:
            name=label+'-'+target;obj=out/(name+'-led_gate.o')
            ccmd=[*cc,'-std=c11',*flags,*extra,*includes,'-c',str(donor),'-o',str(obj)]
            cmd=[*cxx,'-std=c++17',*flags,*extra,*board,*includes,str(fixture),str(driver),str(obj),'-o',str(out/name)]
            with(out/(name+'.build.log')).open('x')as log:
                subprocess.run(ccmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
                subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
            run=[str(out/name)]
            with(out/(name+'.test.log')).open('x')as log:subprocess.run(run,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
            lines=(out/(name+'.test.log')).read_text().splitlines();assert sum(x.startswith('PASS ')for x in lines)==groups and lines[-1]=='NOTIFICATION_GPIO_GROUPS '+str(groups)
            rows.append({'target_branch':target,'enum_variant':label,'groups':groups,'C_donor_command':ccmd,'driver_and_fixture_command':cmd,'execution_command':run,'binary_sha256':sha((out/name).read_bytes()),'test_log_sha256':sha((out/(name+'.test.log')).read_bytes())})
    for name,macro in [('wrong-pin','-DSYNTHETIC_LED_PIN=27'),('wrong-flags','-DSYNTHETIC_LED_FLAGS=1')]:
        command=[*cxx,'-std=c++17',*flags,'-DCONFIG_BOARD_HELTEC_T096=1',macro,*includes,'-c',str(driver),'-o',str(out/(name+'.o'))]
        r=subprocess.run(command,capture_output=True,text=True,timeout=60);(out/(name+'.log')).write_text(r.stdout+r.stderr)
        rejected_assert = 'static assertion failed' in r.stderr or 'static_assert failed' in r.stderr
        assert r.returncode!=0 and rejected_assert and 'synthetic BUILD_ASSERT of actual driver condition' in r.stderr and 'ZephyrNotification.cpp' in r.stderr
        negatives.append({'name':name,'returncode':r.returncode,'command':command,'actual_driver_BUILD_ASSERT_refused':True,'diagnostic_sha256':sha((r.stdout+r.stderr).encode())})
    assert len(rows)==4 and len(negatives)==2 and all(sha(Path(n).read_bytes())==h for n,h in pins.items())
    report.update(status='PASS',all_actual_inputs_still_match=True,GPIO_error_inactive_state_qualified=False,GPIO_error_semantics='readiness freezes and no subsequent renders write; a failed GPIO call can leave either old or committed physical state')
finally:(out/'RESULT.json').write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
print(json.dumps({'status':report['status'],'native_executions':len(rows),'T096_groups_per_enum':9,'headless_groups_per_enum':3,'negative_BUILD_ASSERT_compiles':len(negatives),'hardware':False,'concurrency':False}))
