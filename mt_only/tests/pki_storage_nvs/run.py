#!/usr/bin/env python3
"""Actual pinned NVS + settings functions, production PKI adapters, synthetic NOR only."""
import argparse, ast, hashlib, importlib.util, json, os, pathlib, shlex, subprocess, shutil
HERE=pathlib.Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--zephyr',type=pathlib.Path,required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--cases',type=pathlib.Path,default=HERE/'cases.cpp',help='optional independent case source using the same NOR bridge')
profile=p.add_mutually_exclusive_group()
profile.add_argument('--xiao-expanded-contacts',action='store_true',help='exercise the opt-in banked XIAO contact authority')
profile.add_argument('--t096-fresh-domain',action='store_true',help='use the compact NVS-only T096 production policy')
a=p.parse_args();z=a.zephyr.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
cases=a.cases.resolve()
root=HERE.parents[2];inc=out/'include';mono=root/'zephcore/lib/monocypher';vendor=root/'zephcore/adapters/bench/vendor'
proto=root/'zephcore/bench/upstream-differential/source';(inc/'meshtastic').mkdir(parents=True,exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,inc/'meshtastic'/header.name)
(out/'RESULTS.json').unlink(missing_ok=True)
spec=importlib.util.spec_from_file_location('extract',HERE.parent/'nvs_backend/run.py')
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
tree=ast.parse((HERE.parent/'nvs_capacity/run.py').read_text())
headers=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='headers' for t in n.targets))
headers['zephyr/kernel.h']=r'''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#define K_FOREVER -1
#define K_NO_WAIT 0
#define KB(x) ((x)*1024u)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#ifdef __cplusplus
#define BUILD_ASSERT(c,m) static_assert(c,m)
extern "C" {
#else
#define BUILD_ASSERT(c,m) _Static_assert(c,m)
#endif
#define IS_ENABLED(c) 0
#define ZTESTABLE_STATIC static
#define __packed __attribute__((packed))
struct k_mutex {int depth,owner;};
int model_mutex_lock(struct k_mutex *,int);
int model_mutex_unlock(struct k_mutex *);
static inline void k_mutex_init(struct k_mutex *m){m->depth=m->owner=0;}
static inline int k_mutex_lock(struct k_mutex *m,int t){return model_mutex_lock(m,t);}
static inline int k_mutex_unlock(struct k_mutex *m){return model_mutex_unlock(m);}
#ifdef __cplusplus
}
#endif
'''
headers['zephyr/settings/settings.h']=r'''#pragma once
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
ssize_t settings_get_val_len(const char *);
ssize_t settings_load_one(const char *,void *,size_t);
int settings_delete(const char *);
typedef ssize_t (*settings_read_cb)(void *,void *,size_t);
typedef int (*settings_load_direct_cb)(const char *,size_t,settings_read_cb,void *,void *);
int settings_load_subtree_direct(const char *,settings_load_direct_cb,void *);
#define SETTINGS_FULL_NAME_LEN 96
int settings_save_one(const char *,const void *,size_t);
int settings_storage_get(void **);
#ifdef __cplusplus
}
#endif
'''
# This native NVS model has no generated target DT or history partition.
# Compile the production eraser's disabled branch; never map it onto NOR.
headers['zephyr/devicetree.h']=r'''#pragma once
#define DT_NODELABEL(name) fixture_disabled_node
#define DT_NODE_EXISTS(node) 0
'''
headers['zephyr/storage/flash_map.h']='#pragma once\n'
headers['zephyr/device.h']+=r'''
#include <stdbool.h>
static inline bool device_is_ready(const struct device *dev){(void)dev;return false;}
'''
headers['zephyr/drivers/flash.h']+=r'''
#include <errno.h>
static inline int flash_erase(const struct device *dev,off_t offset,size_t size){
 (void)dev;(void)offset;(void)size;return -ENOTSUP;
}
'''
for name,content in headers.items():
    f=inc/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(content)
nvs=z/'subsys/settings/src/settings_nvs.c';store=z/'subsys/settings/src/settings_store.c'
parts=[(HERE/'preamble.c').read_text(), '#define nvs_read native_nvs_read', '#include '+json.dumps(str(z/'subsys/kvss/nvs/nvs.c')), '#undef nvs_read', r'''
ssize_t nvs_read(struct nvs_fs *fs,uint16_t id,void *data,size_t len){
 ++read_calls;if(read_error&&id==read_id)return read_error;
 return native_nvs_read(fs,id,data,len);
}
''']
for name in ('settings_nvs_read_fn','settings_nvs_load','settings_nvs_save','settings_nvs_backend_init'):
    parts.append(extract.function(nvs.read_text(),name))
for name in ('settings_set_default_cb','settings_get_val_len_default_cb','settings_get_val_len','settings_load_one','settings_save_one','settings_delete','settings_storage_get','settings_load_subtree_direct'):
    parts.append(extract.function(store.read_text(),name))
parts.append((HERE/'bridge.c').read_text());generated=out/'actual_nvs.c';generated.write_text('\n\n'.join(parts))
inputs=[z/'subsys/kvss/nvs/nvs.c',z/'subsys/kvss/nvs/nvs_priv.h',z/'include/zephyr/kvss/nvs.h',nvs,store,
        HERE.parent/'nvs_capacity/run.py',HERE.parent/'nvs_backend/run.py',
        *[f for directory in (root/'mt_only/src',root/'mt_only/experimental/pki',mono,vendor,proto)
          for f in sorted(directory.rglob('*')) if f.is_file() and f.suffix in ('.c','.cpp','.h')],
        *[f for f in sorted(HERE.iterdir()) if f.is_file()],cases]
def label(f):
    if f.is_relative_to(z):return str(f.relative_to(z))
    if f.is_relative_to(root):return str(f.relative_to(root))
    return 'external_cases/'+f.name
digest=lambda: {label(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
before=digest();flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DCONFIG_SETTINGS_NVS=1',
 '-I'+str(inc),'-I'+str(z/'include'),'-I'+str(HERE),'-I'+str(root/'mt_only/src'),'-I'+str(root/'mt_only/experimental/pki'),'-I'+str(mono),'-I'+str(vendor),'-I'+str(proto/'nanopb')]
if a.xiao_expanded_contacts:flags.append('-DCONFIG_MT_XIAO_EXPANDED_CONTACTS=1')
if a.t096_fresh_domain:
    flags+=['-DCONFIG_MT_PKI_FRESH_DOMAIN_ONLY=1','-DCONFIG_MT_PKI_COMPACT_RUNTIME_ONLY=1',
            '-DMT_PKI_LEGACY_TAIL_TOOLS_NATIVE_DISABLED=1']
commands=[];objects=[]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
def run(cmd):
    commands.append(list(map(str,cmd)));result=subprocess.run(cmd,capture_output=True,text=True)
    with (out/'compile.log').open('a') as log:log.write(result.stdout+result.stderr)
    if result.returncode:print(result.stdout+result.stderr);raise SystemExit(result.returncode)
(out/'compile.log').write_text('')
# The typed extended-settings cases call nanopb directly. Compile only its
# pinned runtime plus this repository's descriptor definitions; the upstream
# differential snapshot supplies matching headers but no module_config.pb.c.
for source in (generated,mono/'monocypher.c',mono/'monocypher-ed25519.c',
               proto/'nanopb/pb_common.c',proto/'nanopb/pb_decode.c',proto/'nanopb/pb_encode.c'):
    obj=out/(source.name+'.o');run([*cc,'-std=c11','-Wall','-Wextra','-Wno-unused-function',*flags,'-c',str(source),'-o',str(obj)]);objects.append(obj)
sources=[cases,root/'mt_only/src/pki/PkiIdentity.cpp',root/'mt_only/src/pki/PkiLedger.cpp',root/'mt_only/src/pki/PkiStorage.cpp',root/'mt_only/src/pki/PkiAppendHistory.cpp',root/'mt_only/src/pki/PkiProvisioning.cpp',root/'mt_only/src/pki/ZephyrPkiStorage.cpp',root/'mt_only/src/Protobuf.cpp',vendor/'MtSignature.cpp']
run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,*map(str,sources),*map(str,objects),'-o',str(out/'tests')])
r=subprocess.run([str(out/'tests')],capture_output=True,text=True,timeout=180,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
print(r.stdout+r.stderr,end='');(out/'run.log').write_text(r.stdout+r.stderr)
stable=before==digest();report={'status':'PASS' if r.returncode==0 and stable else 'FAIL','exit':r.returncode,'source_stable':stable,
 'source_sha256':before,'sanitizers':['address','undefined'],'hardware':False,'synthetic_credentials_only':True,
 'limitations':'Byte-addressed NOR and finite before/partial/after fault injection; not physical flash timing, wear, malicious rollback, or arbitrary analog brownout qualification.'}
(out/'RESULTS.json').write_text(json.dumps(report,indent=2)+'\n')
(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
raise SystemExit(r.returncode if stable else 2)
