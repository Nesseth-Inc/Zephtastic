#!/usr/bin/env python3
"""Offline full pinned NVS engine with byte-addressed NOR, actual settings save/init."""
import argparse, hashlib, json, pathlib, shlex, subprocess, re, os
HERE=pathlib.Path(__file__).resolve().parent
p=argparse.ArgumentParser();p.add_argument('--zephyr',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,default=HERE/'out');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
z=a.zephyr.resolve();inc=out/'include'
headers={
'zephyr/kernel.h':r'''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#define K_FOREVER 0
#define KB(x) ((x)*1024u)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BUILD_ASSERT(c,m) _Static_assert(c,m)
#define IS_ENABLED(c) 0
#define ZTESTABLE_STATIC static
#define __packed __attribute__((packed))
struct k_mutex {int depth;};
static inline void k_mutex_init(struct k_mutex *m){m->depth=0;}
static inline void k_mutex_lock(struct k_mutex *m,int t){(void)t;++m->depth;}
static inline void k_mutex_unlock(struct k_mutex *m){assert(m->depth>0);--m->depth;}
''',
'zephyr/device.h':'#pragma once\nstruct device {int unused;};\n',
'zephyr/toolchain.h':'#pragma once\n',
'zephyr/logging/log.h':'''#pragma once
#define LOG_MODULE_REGISTER(...)
#define LOG_DBG(...) do{}while(0)
#define LOG_INF(...) do{}while(0)
#define LOG_WRN(...) do{}while(0)
#define LOG_ERR(...) do{}while(0)
''',
'zephyr/drivers/flash.h':r'''#pragma once
#include <sys/types.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
struct flash_parameters {size_t write_block_size;uint8_t erase_value;};
struct flash_pages_info {off_t start_offset;size_t size,index;};
const struct flash_parameters *flash_get_parameters(const struct device *);
size_t flash_get_write_block_size(const struct device *);
int flash_get_page_info_by_offs(const struct device *,off_t,struct flash_pages_info *);
int flash_read(const struct device *,off_t,void *,size_t);
int flash_write(const struct device *,off_t,const void *,size_t);
int flash_flatten(const struct device *,off_t,size_t);
''',
'zephyr/sys/crc.h':r'''#pragma once
#include <stdint.h>
#include <stddef.h>
static inline uint8_t crc8_ccitt(uint8_t crc,const void *data,size_t n){const uint8_t *p=data;while(n--){crc^=*p++;for(unsigned b=0;b<8;b++)crc=(uint8_t)((crc<<1)^((crc&128)?7:0));}return crc;}
static inline uint32_t crc32_ieee(const void *data,size_t n){const uint8_t *p=data;uint32_t c=~0u;while(n--){c^=*p++;for(unsigned b=0;b<8;b++)c=(c>>1)^((0u-(c&1))&0xedb88320u);}return ~c;}
'''}
for name,s in headers.items():f=inc/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(s)
def fn(src,name):
 m=re.search(r'^(?:static\s+)?int\s+'+name+r'\([^;]*?\)\s*\{',src,re.M|re.S);assert m,name
 at=m.end();d=1
 while d:d+=(src[at]=='{')-(src[at]=='}');at+=1
 return src[m.start():at]
settings=(z/'subsys/settings/src/settings_nvs.c').read_text()
preamble=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/drivers/flash.h>
#define CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define NVS_NAMECNT_ID 0x8000
#define NVS_NAME_ID_OFFSET 0x4000
#define SETTINGS_MAX_NAME_LEN 80
#define SETTINGS_EXTRA_LEN 16
struct settings_store {int unused;};
struct settings_nvs {struct settings_store cf_store;struct nvs_fs cf_nvs;uint16_t last_name_id;const struct device *flash_dev;};
static uint8_t flash_bytes[32768];
static const struct device dev={0};
static const struct flash_parameters parameters={4,255};
static unsigned sectors,reads,writes,erases,events,cut_at;static int cut_phase;static bool cuts;
static jmp_buf power_loss;
const struct flash_parameters *flash_get_parameters(const struct device *d){assert(d==&dev);return &parameters;}
size_t flash_get_write_block_size(const struct device *d){assert(d==&dev);return 4;}
int flash_get_page_info_by_offs(const struct device *d,off_t o,struct flash_pages_info *i){assert(d==&dev);*i=(struct flash_pages_info){o-o%4096,4096,(size_t)o/4096};return 0;}
static void bound(off_t o,size_t n){assert(o>=0&&(size_t)o+n<=sectors*4096);}
int flash_read(const struct device *d,off_t o,void *p,size_t n){assert(d==&dev);bound(o,n);++reads;memcpy(p,flash_bytes+o,n);return 0;}
int flash_write(const struct device *d,off_t o,const void *p,size_t n){assert(d==&dev);bound(o,n);assert(!(o%4)&&!(n%4));++events;++writes;
 bool hit=cuts&&events==cut_at;if(hit&&cut_phase==0)longjmp(power_loss,1);
 size_t count=hit&&cut_phase==1?(n/2/4)*4:n;const uint8_t *b=p;
 for(size_t i=0;i<count;i++){assert((flash_bytes[o+i]&b[i])==b[i]);flash_bytes[o+i]&=b[i];}
 if(hit)longjmp(power_loss,1);return 0;}
int flash_flatten(const struct device *d,off_t o,size_t n){assert(d==&dev);bound(o,n);assert(!(o%4096)&&!(n%4096));++events;++erases;
 bool hit=cuts&&events==cut_at;if(hit&&cut_phase==0)longjmp(power_loss,1);
 memset(flash_bytes+o,255,hit&&cut_phase==1?n/2:n);if(hit)longjmp(power_loss,1);return 0;}
'''
source=preamble+'\n#include '+json.dumps(str(z/'subsys/kvss/nvs/nvs.c'))+'\n'+fn(settings,'settings_nvs_save')+'\n'+fn(settings,'settings_nvs_backend_init')+'\n'+(HERE/'cases.c').read_text()
gen=out/'full_nvs_cases.c';gen.write_text(source);exe=out/'full_nvs_cases'
command=[*shlex.split(os.environ.get('CC','clang')),'-std=c11','-g','-O1','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined','-I'+str(inc),'-I'+str(z/'include'),str(gen),'-o',str(exe)]
p=subprocess.run(command,capture_output=True,text=True);(out/'compile.log').write_text(p.stdout+p.stderr)
if p.returncode:print(p.stderr);raise SystemExit(p.returncode)
r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=90,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/'run.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
inputs={name:hashlib.sha256((z/name).read_bytes()).hexdigest() for name in ['subsys/kvss/nvs/nvs.c','subsys/kvss/nvs/nvs_priv.h','include/zephyr/kvss/nvs.h','subsys/settings/src/settings_nvs.c']}
(out/'RESULTS.json').write_text(json.dumps({'status':'PASS' if not r.returncode else 'FAIL','implementation_sha256':inputs,'sanitizers':['address','undefined'],'simulated_flash_only':True,'limits':'Byte-addressed NOR model and discrete write/erase interruptions; not physical flash endurance, controller timing, or arbitrary analog brownout validation.','exit':r.returncode},indent=2)+'\n');raise SystemExit(r.returncode)
