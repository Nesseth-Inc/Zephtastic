#!/usr/bin/env python3
"""Source-bound deterministic callback review; does not simulate scheduler concurrency."""
import argparse,hashlib,json,os,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
local=Path(__file__).resolve();test=local.with_name('m11_psa_threading_adversarial.c');src=root/'mt_only/src/pki/psa-threading'
for sub in ['zephyr','mbedtls']:(out/sub).mkdir(exist_ok=True)
(out/'zephyr/kernel.h').write_text('''#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define BUILD_ASSERT(c,m) _Static_assert(c,m)
#define K_MSEC(x) (x)
#define K_FOREVER (-1)
struct k_mutex {void *owner;unsigned lock_count;};
struct k_condvar {unsigned opaque;};
void k_panic(void) __attribute__((noreturn));
bool k_is_in_isr(void);void *k_current_get(void);
int k_mutex_init(struct k_mutex *);int k_mutex_lock(struct k_mutex *,int);int k_mutex_unlock(struct k_mutex *);
int k_condvar_init(struct k_condvar *);int k_condvar_signal(struct k_condvar *);int k_condvar_broadcast(struct k_condvar *);int k_condvar_wait(struct k_condvar *,struct k_mutex *,int);
''')
(out/'zephyr/init.h').write_text('#define SYS_INIT(fn,level,priority) static int (*review_init_function)(void)=fn;static const unsigned review_init_priority=priority\n')
(out/'mbedtls/threading.h').write_text('''#pragma once
#include "threading_alt.h"
void mbedtls_threading_set_alt(int (*)(mbedtls_platform_mutex_t *),void (*)(mbedtls_platform_mutex_t *),int (*)(mbedtls_platform_mutex_t *),int (*)(mbedtls_platform_mutex_t *),int (*)(mbedtls_platform_condition_variable_t *),void (*)(mbedtls_platform_condition_variable_t *),int (*)(mbedtls_platform_condition_variable_t *),int (*)(mbedtls_platform_condition_variable_t *),int (*)(mbedtls_platform_condition_variable_t *,mbedtls_platform_mutex_t *));
''')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def state():return {str(v.relative_to(root)):sha(v) for v in sorted(src.rglob('*')) if v.is_file()}
def revision():
 if (root/'.git').exists():return subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
 return None
before=state();head=revision();testhash=sha(test);runnerhash=sha(local)
cc=shlex.split(os.environ.get('CC','clang'))
cmd=[*cc,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',f'-I{out}',f'-I{src}',f'-DMT_REVIEW_THREADING_SOURCE="{src/"ZephyrPsaThreading.c"}"','-DCONFIG_MT_SHARED_MBEDTLS_THREADING=1','-DCONFIG_MULTITHREADING=1','-DCONFIG_MBEDTLS_INIT=1','-DCONFIG_KERNEL_INIT_PRIORITY_DEFAULT=40','-DMBEDTLS_THREADING_C','-DMBEDTLS_THREADING_ALT',str(test),'-o',str(out/'review-tests')]
subprocess.run(cmd,check=True)
r=subprocess.run([out/'review-tests'],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
stable=state()==before and revision()==head and sha(test)==testhash and sha(local)==runnerhash
result={'status':'PASS' if r.returncode==0 and stable else 'FAIL','source_commit':head,'source_stable':stable,'source_sha256':before,'test_sha256':testhash,'runner_sha256':runnerhash,'exit_code':r.returncode,'ASAN_UBSAN':True,'groups':r.stdout.splitlines(),'boundary':'Actual production C callbacks with independent deterministic kernel and PSA installation doubles. Does not prove thread scheduling, target ordering, panic RF effects, or real PSA global locking.','hardware':False,'credentials':False}
(out/'RESULTS.json').write_text(json.dumps(result,indent=2)+'\n');(out/'commands.json').write_text(json.dumps([cmd],indent=2)+'\n')
raise SystemExit(r.returncode if stable else 2)
