"""Root-runnable local proposal tests; no Git/network/hardware execution."""
from pathlib import Path
import argparse,subprocess,os,json,hashlib,shutil
if not __debug__:raise RuntimeError('Optimized BLE qualification refused')
SCRIPT=Path(__file__).resolve();APP=SCRIPT.parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
r=APP/'tests/ble_native_store';s=APP.parent;out=a.output.resolve()
assert not out.exists() and out!=s and s not in out.parents
out.mkdir(parents=True)
def body(path):return path.read_text()
def fn(text,signature):
 start=text.index(signature);opening=text.index('{',start);depth=1;end=opening+1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def run(args,tag,timeout=60):
 done=subprocess.run([str(x)for x in args],capture_output=True,text=True,timeout=timeout,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/(tag+'.stdout')).write_text(done.stdout);(out/(tag+'.stderr')).write_text(done.stderr);rows.append({'tag':tag,'argv':[str(x)for x in args],'returncode':done.returncode});print(tag,done.returncode,flush=True)
 if done.returncode:print(done.stderr[:2000],flush=True);raise RuntimeError(tag)
 return done
def snapshot():return {str(x.relative_to(s)):hashlib.sha256(x.read_bytes()).hexdigest()for x in [SCRIPT,*sorted(r.rglob('*'))]if x.is_file()}
inputs_before=snapshot()
dep_paths=[x for d in ['mt_only/src','zephcore/adapters/bench/vendor','zephcore/lib/monocypher','zephcore/bench/upstream-differential/source','mt_only/dependency-overlay/zephyr/subsys']for x in sorted((s/d).rglob('*'))if x.is_file()and x.suffix in ('.h','.hpp','.c','.cpp')]
dep_paths += [x for x in sorted((s/'mt_only/tests/ble_runtime').rglob('*'))if x.is_file()]
dep_paths += [s/'mt_only/tests/known_channel_fixture.h',s/'mt_only/tests/ble_runtime_cases.cpp']
dep_paths += [APP/'prj.conf',APP/'boards/xiao.conf']
def dependencies():return {str(x.relative_to(s)):hashlib.sha256(x.read_bytes()).hexdigest()for x in dep_paths}
dependencies_before=dependencies()
freeze_sha=hashlib.sha256(json.dumps(dependencies_before,sort_keys=True,separators=(',',':')).encode()).hexdigest()
rows=[]
try:
 proto=s/'zephcore/bench/upstream-differential/source';include=out/'include/meshtastic';include.mkdir(parents=True)
 for h in proto.glob('*.pb.h'):shutil.copyfile(h,include/h.name)
 common=['-g','-fsanitize=address,undefined',*(['-fshort-enums']if a.short_enums else [])]
 inc=['-I'+str(s/'mt_only/src'),'-I'+str(s/'mt_only/tests'),'-I'+str(s/'zephcore/adapters/bench/vendor'),'-I'+str(s/'zephcore/lib/monocypher'),'-I'+str(out/'include'),'-I'+str(proto/'nanopb')]
 objects=[]
 for n,path in [('mono',s/'zephcore/lib/monocypher/monocypher.c'),('ed',s/'zephcore/lib/monocypher/monocypher-ed25519.c'),('pb_common',proto/'nanopb/pb_common.c'),('pb_encode',proto/'nanopb/pb_encode.c'),('pb_decode',proto/'nanopb/pb_decode.c')]:
  obj=out/(n+'.o');run(['clang','-std=c11',*common,*inc,'-c',path,'-o',obj],'build-'+n);objects.append(obj)
 cpp=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',*common,*inc]
 module=s/'mt_only/src/BleSettingsAdmission.cpp';stubs=['-I'+str(r/'stubs')]
 deps=[s/'mt_only/src/Protobuf.cpp',s/'zephcore/adapters/bench/vendor/MtCodec.cpp',s/'zephcore/adapters/bench/vendor/MtSignature.cpp',*objects]
 run([*cpp,*stubs,r/'admission.cpp',module,*deps,'-o',out/'admission'],'build-admission')
 for case in ['safe','unarmed','native_busy','raced_rx','nested_delete','mt_before_grant','timeout','late_grant','hard_pause','restore_failure','io_5','io_28','io_12','io_16','session','strict','device_reset','identity','cancel_prepared','cancel_prepared_immediate','cancel_running','overlap','slot_timeout']:
  run([out/'admission',case],'admission-'+case,8)
 run([*cpp,r/'bond_acceptance.cpp','-o',out/'bond'],'build-bond');run([out/'bond'],'bond')
 # Extract exact full integrated SDK bodies, never a handwritten substitute.
 sdk=s/'mt_only/dependency-overlay/zephyr/subsys';strict=fn(body(sdk/'settings/src/settings.c'),'int settings_call_set_handler(')+'\n'+fn(body(sdk/'settings/src/settings_store.c'),'int settings_load_subtree(')+'\n'+fn(body(sdk/'settings/src/settings_store.c'),'int settings_load_subtree_direct(')+'\n'
 (out/'actual_strict_load.inc').write_text(strict)
 run([*cpp,'-I'+str(out),r/'strict_load.cpp','-o',out/'strict'],'build-strict');run([out/'strict'],'strict-load-backend-handler-direct')
 (out/'actual_key_store.inc').write_text(fn(body(sdk/'bluetooth/host/keys.c'),'int bt_keys_store(')+'\n'+fn(body(sdk/'bluetooth/host/keys.c'),'void bt_keys_clear(')+'\n')
 # The real native GATT commit may write its hash only after all reads validate.
 (out/'exact_gatt.inc').write_bytes((r/'upstream_gatt_boot.inc').read_bytes())
 loader=body(sdk/'settings/src/settings_store.c')
 weak=fn(loader,'__weak int zt_settings_prepare_commit(')
 (out/'exact_settings.inc').write_text(weak+'\n'+strict)
 (out/'exact_weak.inc').write_text(weak+'\n'+fn(loader,'int settings_load_subtree(')+'\n')
 run([*cpp,*stubs,'-I'+str(out),'-DCONFIG_MT_BLE_NATIVE_STORE=1',r/'actual_gatt_boot.cpp',module,'-o',out/'gatt-boot'],'build-gatt-boot')
 for case in ('missing','changed','matching','backend_first','backend_second','handler_failure','cleanup_failure','cleanup_ignored','commit_failure','write_failure','runtime_hook','worker_hook','wrong_subtree','missing_prepare','direct_no_commit'):
  run([out/'gatt-boot',case],'gatt-boot-'+case,10)
 run([*cpp,'-I'+str(out),r/'weak_default.cpp','-o',out/'weak-default'],'build-weak-default')
 run([out/'weak-default'],'weak-default-other-subtrees')
 # Actual native identity/name/IRK/appearance and peer-name handlers, not a
 # generic replacement. Synthetic backend/identity counters test no replacement
 # commit on a present malformed record; genuine absence retains native setup.
 native_functions=fn(body(r/'upstream_hex.c'),'int char2hex(')+'\n'+fn(body(r/'upstream_hex.c'),'size_t hex2bin(')+'\n'
 for signature in ('int bt_settings_decode_key(','static int set_setting(','static int commit_settings('):
  native_functions+=fn(body(sdk/'bluetooth/host/settings.c'),signature)+'\n'
 native_functions+=fn(body(sdk/'settings/src/settings.c'),'int settings_call_set_handler(')+'\n'+fn(body(sdk/'settings/src/settings_store.c'),'int settings_load_subtree(')+'\n'
 (out/'actual_native_handlers.inc').write_text(native_functions)
 for slots,name_max in ((1,29),(1,248),(2,29),(4,29)):
  tag='native-handlers-'+str(slots)+'-'+str(name_max)
  run([*cpp,'-I'+str(out),'-DCONFIG_BT_ID_MAX='+str(slots),'-DCONFIG_BT_DEVICE_NAME_MAX='+str(name_max),r/'native_handlers.cpp','-o',out/tag],'build-'+tag)
  run([out/tag],tag)
 # Extend retained MT stack double only at the new native API boundaries.
 old=body(s/'mt_only/tests/ble_runtime/ZephyrMock.h');start=old.index('inline unsigned sessionLockDepth');end=old.index('inline void sys_put_le32')
 msg=old[old.index('struct k_msgq'):old.index('using atomic_t')]
 mock=old[:start]+'#include <ThreadKernel.h>\n#include <zephyr/sys/atomic.h>\n#define sessionLockDepth kernelMutexDepth\n'+msg+old[end:]
 peer='struct bt_addr_le_t{uint8_t type=0;uint8_t bytes[6]{1,2,3,4,5,6};};\ninline bt_addr_le_t mockPeer;\ninline int bt_addr_le_cmp(const bt_addr_le_t*a,const bt_addr_le_t*b){return memcmp(a,b,sizeof(*a));}\n'
 mock=mock.replace('struct bt_conn;',peer+'struct bt_conn;')
 mock=mock.replace('void(*security_changed)(bt_conn*,bt_security_t,bt_security_err);','void(*identity_resolved)(bt_conn*,const bt_addr_le_t*,const bt_addr_le_t*);void(*security_changed)(bt_conn*,bt_security_t,bt_security_err);')
 mock=mock.replace('struct bt_conn{bt_security_t','struct bt_conn{bt_addr_le_t peer=mockPeer;bool bondable=false;bt_security_t')
 mock+='''
#define BT_ID_DEFAULT 0
inline bool mockBootBond=false,mockIdentityPending=false;
inline unsigned mockBootBondCount=0;
inline int mockLoadResult=0;
struct bt_bond_info{bt_addr_le_t addr;};
inline const bt_addr_le_t*bt_conn_get_dst(bt_conn*p){return &p->peer;}
inline int bt_conn_set_bondable(bt_conn*p,bool enabled){stackBoundary();p->bondable=enabled;return 0;}
inline void bt_id_get(bt_addr_le_t*out,size_t*count){if(*count)*out=mockPeer;*count=1;}
inline void bt_foreach_bond(uint8_t,void(*cb)(const bt_bond_info*,void*),void*ctx){unsigned count=mockBootBond?1:mockBootBondCount;for(unsigned i=0;i<count;++i){bt_bond_info info{mockPeer};info.addr.bytes[0]=uint8_t(1+i);cb(&info,ctx);}}
extern "C" int zt_settings_prepare_commit(const char*);
inline int settings_load_subtree(const char*subtree){return mockLoadResult?mockLoadResult:zt_settings_prepare_commit(subtree);}
'''
 ts=out/'transport-stubs';(ts/'zephyr/bluetooth').mkdir(parents=True);(ts/'zephyr/sys').mkdir();(ts/'zephyr/settings').mkdir()
 (ts/'WorkQueueMock.h').write_bytes((s/'mt_only/tests/ble_runtime/WorkQueueMock.h').read_bytes())
 (ts/'ThreadKernel.h').write_bytes((r/'stubs/zephyr/kernel.h').read_bytes());(ts/'NativeTransportMock.h').write_text(mock)
 for name in ['kernel.h','bluetooth/bluetooth.h','bluetooth/conn.h','bluetooth/gatt.h','settings/settings.h','sys/byteorder.h']:(ts/'zephyr'/name).write_text('#include "NativeTransportMock.h"\n')
 (ts/'zephyr/sys/atomic.h').write_bytes((r/'stubs/zephyr/sys/atomic.h').read_bytes())
 defines=['-DCONFIG_MT_BLE_NATIVE_STORE=1','-DCONFIG_BT_SETTINGS=1','-DCONFIG_BT_BONDABLE=1','-DCONFIG_BT_BONDABLE_PER_CONNECTION=1','-DCONFIG_BT_MAX_CONN=1','-DCONFIG_BT_MAX_PAIRED=5','-DCONFIG_BT_ID_MAX=1','-DCONFIG_MT_DISPLAY=1']
 # Pinned Zephyr Kconfig.gatt defaults peripheral auto-security to y. Apply
 # the same common then board fragments as the ordinary XIAO build.
 auto_security='y'
 for path in (APP/'prj.conf',APP/'boards/xiao.conf'):
  for line in path.read_text().splitlines():
   if line.startswith('CONFIG_BT_GATT_AUTO_SEC_REQ='):
    auto_security=line.split('=',1)[1]
 assert auto_security in ('y','n')
 defines.append('-DZT_TEST_XIAO_GATT_AUTO_SEC_REQ='+('1' if auto_security=='y' else '0'))
 run([*cpp,'-I'+str(ts),'-I'+str(out),'-I'+str(s/'mt_only/src'),*defines,r/'transport.cpp',module,'-o',out/'transport'],'build-transport')
 for case in ('restored_ccc_central','restored_ccc_absent','restored_ccc_peripheral','restored_ccc_missing_key'):
  run([out/'transport',case],'transport-'+case,8)
 for case in ['nopin_ephemeral_clear','nopin_known_clear','fresh_pin_ephemeral_clear','nopin_clear_failure','boot_failure','queued_identity','nopin','loaded_reconnect','new_pair','unbonded_pair','store_before_pair','wrong_peer','session_store_rejected','failed_store_reconnect','five_peers','overflow_peers','resolved_after_connect','admin_healthy_store','deleted_peer_repair','delete_failure']:
  run([out/'transport',case],'transport-'+case,8)
 # M103 transport callback regressions with native feature absent.
 baseline=(s/'mt_only/tests/ble_runtime_cases.cpp').read_text().replace('#include "../src/BleTransport.cpp"','#include "'+str(s/'mt_only/src/BleTransport.cpp')+'"')
 (out/'baseline.cpp').write_text(baseline)
 run([*cpp,'-I'+str(s/'mt_only/tests/ble_runtime/stubs'),'-I'+str(s/'mt_only/tests/ble_runtime'),'-DCONFIG_MT_DISPLAY=1',out/'baseline.cpp','-o',out/'baseline'],'build-m103-regression');run([out/'baseline'],'m103-callback-regression')
 status='PASS_INTEGRATED_NATIVE_BLE_FIXTURES'
except Exception as error:
 status='FAIL_INTEGRATED_NATIVE_BLE_FIXTURES';print(type(error).__name__,str(error),flush=True)
finally:
 files=snapshot()
 if files!=inputs_before or dependencies()!=dependencies_before:status='FAIL_INPUTS_CHANGED_DURING_FIXTURES'
 (out/'RESULT.json').write_text(json.dumps({'status':status,'commands':rows,'inputs':files,'schema':'zt-native-ble-store-host-tests/v1','integrated_source_manifest_sha256':freeze_sha,'short_enums':a.short_enums,'source_dependencies':dependencies_before,'all_inputs_stable':files==inputs_before and dependencies()==dependencies_before,'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'hardware':False,'target_build':False,'Zephyr_scheduler':False,'physical_bonds':False,'actual_SMP_crypto':False},indent=2,sort_keys=True)+'\n')
raise SystemExit(0 if status.startswith('PASS')else 1)
