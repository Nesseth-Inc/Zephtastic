#!/usr/bin/env python3
"""Offline CLI/version/diagnostic build-boundary checks. No firmware or device."""
import argparse,json,os,re,shlex,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--zephyr',type=Path,required=True);a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
cxx=shlex.split(os.environ.get('CXX','c++'))
ordinary='2.8.2.ZT-m188'
def checked(cmd,**kwargs):
 kwargs['check']=True
 try:return subprocess.run(cmd,**kwargs)
 except subprocess.CalledProcessError as error:
  sys.stderr.write(f"command failed ({error.returncode}): {shlex.join(map(str,cmd))}\n")
  if error.stdout:sys.stderr.write(error.stdout if isinstance(error.stdout,str)else error.stdout.decode(errors='replace'))
  if error.stderr:sys.stderr.write(error.stderr if isinstance(error.stderr,str)else error.stderr.decode(errors='replace'))
  raise
shim=out/'bin';shim.mkdir(exist_ok=True)
sdk=out/'sdk';toolbin=sdk/'fake-target/bin';toolbin.mkdir(parents=True,exist_ok=True)
for name in ('target-gcc','target-g++'):
 (toolbin/name).write_text('#!/bin/sh\necho "ZT build-boundary fake SDK compiler 1"\n');(toolbin/name).chmod(0o755)
(shim/'cmake').write_text(f'''#!{sys.executable}
import sys
from pathlib import Path
if '-B' in sys.argv:
 out=Path(sys.argv[sys.argv.index('-B')+1]);out.mkdir(parents=True,exist_ok=True)
 out.joinpath('CMakeCache.txt').write_text('CMAKE_C_COMPILER:FILEPATH={toolbin/'target-gcc'}\\nCMAKE_CXX_COMPILER:FILEPATH={toolbin/'target-g++'}\\n')
''');(shim/'cmake').chmod(0o755)
checks=[]
for board in ('xiao','t096'):
 for diag in (False,True):
  for tx in (False,True):
   profile=f'{board}-{int(diag)}-{int(tx)}';build=out/profile
   cmd=[sys.executable,str(app/'scripts/build-local.py'),board,'--zephyr',str(a.zephyr.resolve()),'--modules',str(out/'modules'),'--nordic-modules',str(out/'nordic'),'--sdk',str(sdk),'--build',str(build),'--source-date-epoch','0']
   if diag:cmd+=['--ble-diagnostics']
   cmd+=['--normal-tx' if tx else '--rx-only']
   checked(cmd,capture_output=True,text=True,env={**os.environ,'PATH':str(shim)+os.pathsep+os.environ['PATH']})
   flags=json.loads((build/'configure-command.json').read_text())
   assert '-DPython3_EXECUTABLE='+sys.executable in flags
   assert '-DCONFIG_MT_BLE_DIAGNOSTICS='+('y' if diag else 'n') in flags
   assert '-DCONFIG_MT_ALLOW_TX='+('y' if tx else 'n') in flags
   assert '-DCONFIG_MT_T096_OWNER_TX='+('y' if tx and board=='t096' else 'n') in flags
   assert '-DCONFIG_MT_NORMAL_TX_DEFAULT='+('y' if tx else 'n') in flags
   expected=ordinary+('-tx'if tx else'-rx')if not diag else'2.8.2.ZT-m188'+('-txd'if tx else'-rxd')
   source='#include "Version.h"\n#include <string_view>\nstatic_assert(std::string_view(mt::FirmwareVersion)=="'+expected+'");\n'
   c=[*cxx,'-std=c++17','-x','c++','-fsyntax-only','-I'+str(app/'src'),'-']
   if diag:c+=['-DCONFIG_MT_BLE_DIAGNOSTICS=1']
   if tx:c+=['-DCONFIG_MT_ALLOW_TX=1','-DCONFIG_MT_NORMAL_ENDPOINT=1']
   checked(c,input=source,text=True,capture_output=True)
   checks.append(profile)
print('PASS eight board/diagnostic/TX CLI profiles preserve explicit gates and distinct versions')

one=out/'xiao-one-packet';one_cmd=[sys.executable,str(app/'scripts/build-local.py'),'xiao','--zephyr',str(a.zephyr.resolve()),'--modules',str(out/'modules'),'--sdk',str(sdk),'--build',str(one),'--one-packet-tx','--source-date-epoch','0']
checked(one_cmd,capture_output=True,text=True,env={**os.environ,'PATH':str(shim)+os.pathsep+os.environ['PATH']})
one_flags=json.loads((one/'configure-command.json').read_text());one_provenance=json.loads((one/'build-provenance-private.json').read_text())
assert '-DCONFIG_MT_ALLOW_TX=y' in one_flags and '-DCONFIG_MT_NORMAL_ENDPOINT=n' in one_flags
assert '-DCONFIG_MT_NORMAL_TX_DEFAULT=n' in one_flags and '-DCONFIG_MT_T096_OWNER_TX=n' in one_flags
assert one_provenance['profile']=='one-packet-tx' and one_provenance['one_packet_tx'] is True
assert one_provenance['normal_tx'] is False and one_provenance['rx_only'] is False
one_source='#include "Version.h"\n#include <string_view>\nstatic_assert(std::string_view(mt::FirmwareVersion)=="2.8.2.ZT-m188-1tx");\n'
checked([*cxx,'-std=c++17','-x','c++','-fsyntax-only','-I'+str(app/'src'),'-DCONFIG_MT_ALLOW_TX=1','-'],input=one_source,text=True,capture_output=True)
checks.append('xiao-one-packet')
print('PASS legacy one-packet TX has distinct flags, provenance and firmware identity')

private_normal=[sys.executable,str(app/'scripts/build-local.py'),'xiao','--zephyr',str(a.zephyr.resolve()),'--modules',str(out/'modules'),'--sdk',str(sdk),'--build',str(out/'private-normal-rejected'),'--normal-tx','--target-private-runtime','--source-date-epoch','0']
r=subprocess.run(private_normal,capture_output=True,text=True,env={**os.environ,'PATH':str(shim)+os.pathsep+os.environ['PATH']})
assert r.returncode!=0 and 'Private runtime remains load-only' in r.stderr
private_one=private_normal.copy();private_one[private_one.index('--normal-tx')]='--one-packet-tx';private_one[private_one.index(str(out/'private-normal-rejected'))]=str(out/'private-one-rejected')
r=subprocess.run(private_one,capture_output=True,text=True,env={**os.environ,'PATH':str(shim)+os.pathsep+os.environ['PATH']})
assert r.returncode!=0 and 'Private runtime requires the normal endpoint' in r.stderr
print('PASS private load-only runtime rejects the normal-TX profile before configuration')
# The opt-in observer may accompany only the existing T096 normal stock lane.
for diag in (False,True):
 build=out/('stock-trial-'+str(int(diag)))
 cmd=[sys.executable,str(app/'scripts/build-local.py'),'t096','--zephyr',str(a.zephyr.resolve()),'--modules',str(out/'modules'),'--nordic-modules',str(out/'nordic'),'--sdk',str(sdk),'--build',str(build),'--source-date-epoch','0','--normal-tx','--stock-pki-trial']
 if diag:cmd+=['--ble-diagnostics']
 checked(cmd,capture_output=True,text=True,env={**os.environ,'PATH':str(shim)+os.pathsep+os.environ['PATH']})
 cfg=json.loads((build/'configure-command.json').read_text());prov=json.loads((build/'build-provenance-private.json').read_text())
 assert '-DCONFIG_MT_PKI_STOCK_TRIAL=y' in cfg and '-DCONFIG_MAIN_STACK_SIZE=16384' in cfg
 assert '-DCONFIG_MT_BLE_DIAGNOSTICS='+('y'if diag else'n') in cfg
 assert prov['stock_pki_trial'] is True and prov['ble_diagnostics'] is diag
for tag,board,options in [
 ('wrong-board','xiao',['--normal-tx']),('no-normal','t096',[]),
 ('resource','t096',['--normal-tx','--runtime-resources']),
 ('private','t096',['--normal-tx','--target-private-runtime']),
 ('one-packet','xiao',['--one-packet-tx']),('legacy-owner','t096',['--t096-owner-tx'])]:
 build=out/('rejected-stock-'+tag)
 cmd=[sys.executable,str(app/'scripts/build-local.py'),board,'--zephyr',str(a.zephyr.resolve()),'--modules',str(out/'modules'),'--sdk',str(sdk),'--build',str(build),'--source-date-epoch','0','--stock-pki-trial','--ble-diagnostics',*options]
 result=subprocess.run(cmd,capture_output=True,env={**os.environ,'PATH':str(shim)+os.pathsep+os.environ['PATH']})
 assert result.returncode!=0 and not build.exists(),tag
print('PASS stock-trial normal/diagnostic profiles and six incompatible profile rejections')


cmake=shutil.which('cmake');assert cmake
rules=app/'ble-diagnostics.cmake'
def guard(name,enabled,root,settings='',success=True):
 script=out/(name+'.cmake');record=out/(name+'.calls');record.write_text('')
 script.write_text(f'''set(CONFIG_MT_BLE_DIAGNOSTICS {'ON' if enabled else 'OFF'})
set(CONFIG_BT_MAX_CONN 1)
set(ZEPHYR_BASE "{root}")
{settings}
function(target_sources)
 file(APPEND "{record}" "sources:${{ARGN}}\\n")
endfunction()
function(target_include_directories)
 file(APPEND "{record}" "includes:${{ARGN}}\\n")
endfunction()
function(target_link_options)
 file(APPEND "{record}" "links:${{ARGN}}\\n")
endfunction()
include("{rules}")
''')
 r=subprocess.run([cmake,'-P',str(script)],capture_output=True,text=True)
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 assert (r.returncode==0)==success,(name,r.stdout,r.stderr)
 return record.read_text()
assert guard('disabled',False,out/'absent')==''
active=guard('enabled',True,a.zephyr.resolve())
assert 'src/BleSmpTrace.c' in active and active.count('--wrap=bt_l2cap_recv')==1 and active.count('--wrap=bt_l2cap_send_pdu')==1
for setting in ('CONFIG_BT_CLASSIC','CONFIG_LTO','CONFIG_BT_BONDABLE','CONFIG_BT_SETTINGS','CONFIG_BT_SMP_LOG_LEVEL_DBG','CONFIG_BT_KEYS_LOG_LEVEL_DBG','CONFIG_BT_LOG_SNIFFER_INFO'):
 assert guard(setting,True,a.zephyr.resolve(),f'set({setting} ON)',False)==''
native=guard('qualified-native-store',True,a.zephyr.resolve(),
 'set(CONFIG_MT_BLE_NATIVE_STORE ON)\nset(CONFIG_BT_SETTINGS ON)\nset(CONFIG_BT_BONDABLE ON)')
assert native==active
for setting in ('CONFIG_BT_SMP_LOG_LEVEL_DBG','CONFIG_BT_KEYS_LOG_LEVEL_DBG','CONFIG_BT_LOG_SNIFFER_INFO'):
 assert guard('native-'+setting,True,a.zephyr.resolve(),
  'set(CONFIG_MT_BLE_NATIVE_STORE ON)\nset(CONFIG_BT_SETTINGS ON)\nset(CONFIG_BT_BONDABLE ON)\nset('+setting+' ON)',False)==''
assert guard('multiple-connections',True,a.zephyr.resolve(),'set(CONFIG_BT_MAX_CONN 2)',False)==''
copy=out/'changed-zephyr'
for relative in re.findall(r'file\(SHA256 "\$\{ZEPHYR_BASE\}/([^\"]+)"',rules.read_text()):
 dest=copy/relative;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(a.zephyr/relative,dest)
part=copy/'subsys/bluetooth/host/l2cap.c';part.write_text(part.read_text()+'\n/* diagnostic guard fixture */\n')
assert guard('source-drift',True,copy,success=False)==''
print('PASS opt-in only wrapper linkage; source drift, LTO, multi-connection and sensitive settings fail closed')
# Check every identity branch, including private TX identities whose build gates
# deliberately reject deployment. Compile only; this does not enable those gates.
version_profiles=[]
for private in (False,True):
 for diag in (False,True):
  for mode in ('rx','one','normal'):
   suffix = ({'rx':'prd' if diag else 'prx','one':'p1d' if diag else 'p1t','normal':'ptd' if diag else 'ptx'} if private else {'rx':'rxd' if diag else 'rx','one':'1td' if diag else '1tx','normal':'txd' if diag else 'tx'})[mode]
   expected=ordinary+('-tx'if mode=='normal'else'-rx')if not private and not diag and mode!='one'else'2.8.2.ZT-m188-'+suffix
   defines=[]
   if private:defines+=['-DCONFIG_MT_PKI_TARGET_RUNTIME=1']
   if diag:defines+=['-DCONFIG_MT_BLE_DIAGNOSTICS=1']
   if mode!='rx':defines+=['-DCONFIG_MT_ALLOW_TX=1']
   if mode=='normal':defines+=['-DCONFIG_MT_NORMAL_ENDPOINT=1']
   source='#include "Version.h"\n#include <string_view>\nstatic_assert(std::string_view(mt::FirmwareVersion)=="'+expected+'");\n'
   checked([*cxx,'-std=c++17','-x','c++','-fsyntax-only','-I'+str(app/'src'),*defines,'-'],input=source,text=True,capture_output=True)
   version_profiles.append(expected)
assert len(set(version_profiles))==12
print('PASS all 12 ordinary/private/diagnostic firmware identity branches')
# Resource diagnostics are separate M186 identities on both RX and TX profiles.
resource_profiles=[]
for tx in (False,True):
 expected='2.8.2.ZT-m188-'+('txr' if tx else 'rxr')
 defines=['-DCONFIG_MT_RUNTIME_RESOURCES=1']
 if tx:defines+=['-DCONFIG_MT_ALLOW_TX=1','-DCONFIG_MT_NORMAL_ENDPOINT=1']
 source='#include "Version.h"\n#include <string_view>\nstatic_assert(std::string_view(mt::FirmwareVersion)=="'+expected+'");\n'
 checked([*cxx,'-std=c++17','-x','c++','-fsyntax-only','-I'+str(app/'src'),*defines,'-'],input=source,text=True,capture_output=True)
 resource_profiles.append(expected)
print('PASS two distinct M186 resource diagnostic identities')
for diag in (False,True):
 expected='2.8.2.ZT-m188d1' if diag else ordinary
 defines=['-DCONFIG_MT_PKI_STOCK_TRIAL=1','-DCONFIG_MT_ALLOW_TX=1','-DCONFIG_MT_NORMAL_ENDPOINT=1']
 if diag:defines+=['-DCONFIG_MT_BLE_DIAGNOSTICS=1']
 source='#include "Version.h"\n#include <string_view>\nstatic_assert(std::string_view(mt::FirmwareVersion)=="'+expected+'");\n'
 checked([*cxx,'-std=c++17','-x','c++','-fsyntax-only','-I'+str(app/'src'),*defines,'-'],input=source,text=True,capture_output=True)
print('PASS normal stock-trial identity and distinct opt-in diagnostic identity')

gnss_rules=app/'gnss-native.cmake'
def gnss_guard(name,enabled,root,success):
 script=out/(name+'.cmake');script.write_text('set(CONFIG_MT_GNSS_T096 '+('ON'if enabled else'OFF')+')\ninclude("'+str(gnss_rules)+'")\n')
 r=subprocess.run([cmake,'-P',str(script)],capture_output=True,text=True,env={**os.environ,'ZEPHYR_BASE':str(root)})
 (out/(name+'.log')).write_text(r.stdout+r.stderr);assert (r.returncode==0)==success
gnss_guard('gnss-absent',False,out/'no-gnss',True)
gnss_guard('gnss-qualified',True,app/'dependency-overlay/zephyr',True)
changed_gnss=out/'changed-gnss'
for name in ('gnss_nmea_generic.c','gnss_nmea0183.c','gnss_nmea0183_match.c'):
 dest=changed_gnss/'drivers/gnss'/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(app/'dependency-overlay/zephyr/drivers/gnss'/name,dest)
part=changed_gnss/'drivers/gnss/gnss_nmea0183_match.c';part.write_text(part.read_text()+'\n/* deliberate GNSS source drift */\n')
gnss_guard('gnss-drift',True,changed_gnss,False)
print('PASS absent GNSS skips dependency access; qualified bodies pass and matcher drift refuses')

(out/'results.json').write_text(json.dumps({'status':'PASS','hardware':False,'firmware_compiled':False,'profiles':checks,'version_profiles':version_profiles,'resource_profiles':resource_profiles,'build_boundary_groups':5},indent=2)+'\n')
