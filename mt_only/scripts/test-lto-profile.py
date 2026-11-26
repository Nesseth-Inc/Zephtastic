#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Actual CMake/compiler guard controls over explicit mock SDK inputs; no target/device."""
if not __debug__:raise RuntimeError('optimized tests refused')
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
app=Path(__file__).resolve().parents[1];root=app.parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve()
if out.exists()or out.is_relative_to(root):p.error('fresh output outside source required')
out.mkdir(parents=True);sha=lambda b:hashlib.sha256(b).hexdigest()
selected=[app/'controller-lto-profile.cmake',app/'pki-groundwork.cmake',app/'src/BleTransport.cpp',app/'Kconfig',app/'src/Version.h',app/'tests/lto_controller_sources.json',app/'scripts/build-local.py',Path(__file__).resolve()]
before={str(n.relative_to(root)):sha(n.read_bytes())for n in selected};rows=[]
result={'status':'FAIL','hardware':False,'network':False,'target_build':False,'actual_CMake_generation_only':True,'source_sha256':before,'controls':rows}
try:
 groups=json.loads((app/'tests/lto_controller_sources.json').read_text())['groups'];assert sorted(map(len,groups.values()))==[11,31]
 for case in ['enabled','ordinary','missing-controller','missing-nordic','missing-source','extra-source','genexpr-enabled','genexpr-disabled']:
  folder=out/case;folder.mkdir();sdk=folder/'mock-sdk'
  lines=['cmake_minimum_required(VERSION 3.20)','project(ProfileControl C)','set(CMAKE_EXPORT_COMPILE_COMMANDS ON)','set(ZEPHYR_BASE "'+str(sdk)+'")','set(CONFIG_MT_LTO_RUNTIME '+('OFF'if case=='ordinary'else'ON')+')','add_library(zephyr_interface INTERFACE)','target_compile_options(zephyr_interface INTERFACE -flto=auto -Os)','include("'+str(app/'controller-lto-profile.cmake')+'")']
  for index,(target,paths)in enumerate(groups.items()):
   directory=folder/str(index);directory.mkdir();listed=list(paths)
   if case=='missing-source'and index==0:listed=listed[1:]
   if case in ['extra-source','genexpr-enabled','genexpr-disabled']and index==0:listed+=['unexpected.c']
   for i,n in enumerate(listed):q=sdk/n;q.parent.mkdir(parents=True,exist_ok=True);q.write_text('int fixture_'+str(index)+'_'+str(i)+'(void){return 0;}\n')
   absent=(case=='missing-controller'and target=='subsys__bluetooth__controller')or(case=='missing-nordic'and target.endswith('__nordic'))
   expressions=[]
   for n in listed:
    entry='${ZEPHYR_BASE}/'+n
    if case in ['genexpr-enabled','genexpr-disabled']and index==0 and n=='unexpected.c':entry='$<$<BOOL:'+('1'if case=='genexpr-enabled'else'0')+'>:'+entry+'>'
    expressions.append(' "'+entry+'"')
   text=['add_library('+target+' STATIC']+expressions+[')','target_link_libraries('+target+' PRIVATE zephyr_interface)']+['set_property(SOURCE "${ZEPHYR_BASE}/'+n+'" APPEND PROPERTY COMPILE_OPTIONS -DMT_KEEP=1)'for n in listed]
   (directory/'CMakeLists.txt').write_text('\n'.join(text)+'\n'if not absent else'# absent\n');lines+=['add_subdirectory('+str(index)+')']
  (folder/'CMakeLists.txt').write_text('\n'.join(lines)+'\n');cmd=['cmake','-S',str(folder),'-B',str(folder/'build'),'-G','Unix Makefiles'];r=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
  (folder/'configure.log').write_text(r.stdout+r.stderr);expected=case in ['enabled','ordinary'];assert (r.returncode==0)==expected,(case,r.stderr)
  row={'case':case,'command':cmd,'returncode':r.returncode,'expected_success':expected}
  if expected:
   commands=json.loads((folder/'build/compile_commands.json').read_text());assert len(commands)==42
   actual=[]
   for entry in commands:
    tokens=entry.get('arguments')or shlex.split(entry['command']);optimization=[t for t in tokens if t in ['-Os','-O2']];lto=[t for t in tokens if t=='-fno-lto'or t.startswith('-flto')]
    assert optimization==(['-Os','-O2']if case=='enabled'else['-Os']);assert lto[-1]==('-fno-lto'if case=='enabled'else'-flto=auto')and tokens.count('-DMT_KEEP=1')==1
    actual.append(Path(entry['file']).relative_to(sdk).as_posix())
   assert set(actual)=={n for names in groups.values()for n in names};row['exact_42_sources_final_flags_and_existing_option']=True
  rows.append(row)
 base={'CONFIG_MT_PKI_GROUNDWORK':'ON','CONFIG_MT_PKI_TARGET_RUNTIME':'ON','CONFIG_MT_SHARED_MBEDTLS_THREADING':'ON','CONFIG_MULTITHREADING':'ON','CONFIG_MBEDTLS_USER_CONFIG_ENABLE':'ON','CONFIG_MBEDTLS_USER_CONFIG_FILE':'mt_pki_psa_threading_config.h','CONFIG_MT_LTO_RUNTIME':'ON','CONFIG_BOARD_HELTEC_T096':'ON','CONFIG_LTO':'ON','CONFIG_ISR_TABLES_LOCAL_DECLARATION':'ON','CONFIG_MT_PKI_STOCK_TRIAL':'ON','CONFIG_MT_BLE_NATIVE_STORE':'ON','CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE':'mt_pki_psa_threading_config.h','CONFIG_MBEDTLS_INIT':'ON','CONFIG_KERNEL_INIT_PRIORITY_DEFAULT':'40','CONFIG_MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG':'ON'}
 guard_cases=[('exact',{},True),('ordinary',{'CONFIG_MT_LTO_RUNTIME':'OFF','CONFIG_LTO':'OFF'},True),('unqualified-LTO',{'CONFIG_MT_LTO_RUNTIME':'OFF'},False),('PSA-persistent',{'CONFIG_MBEDTLS_PSA_CRYPTO_STORAGE_C':'ON'},False),('missing-shared-threading',{'CONFIG_MT_SHARED_MBEDTLS_THREADING':'OFF'},False)]
 guard_cases += [('missing-'+n,{n:'OFF'},False)for n in ['CONFIG_BOARD_HELTEC_T096','CONFIG_LTO','CONFIG_ISR_TABLES_LOCAL_DECLARATION','CONFIG_MT_PKI_STOCK_TRIAL','CONFIG_MT_BLE_NATIVE_STORE']]
 for name,changes,expected in guard_cases:
  settings={**base,**changes};script=out/(name+'.cmake')
  stubs='\n'.join('function('+n+')\nendfunction()'for n in ['target_include_directories','target_sources','target_link_options','set_source_files_properties','zephyr_include_directories'])
  script.write_text('\n'.join('set('+n+' "'+v+'")'for n,v in settings.items())+'\n'+stubs+'\ninclude("'+str(app/'pki-groundwork.cmake')+'")\n')
  cmd=['cmake','-P',str(script)];r=subprocess.run(cmd,capture_output=True,text=True,timeout=15);(out/(name+'.log')).write_text(r.stdout+r.stderr);assert (r.returncode==0)==expected,(name,r.stderr);rows.append({'case':'PKI-'+name,'command':cmd,'returncode':r.returncode,'expected_success':expected})
 # The same application-owned backend/config path must be selectable for a
 # WiFi TLS consumer without enabling PKI storage or identity runtime.
 gateway_base={'CONFIG_MT_MQTT_GATEWAY':'ON','CONFIG_MT_SHARED_MBEDTLS_THREADING':'ON','CONFIG_MULTITHREADING':'ON','CONFIG_MBEDTLS_INIT':'ON','CONFIG_KERNEL_INIT_PRIORITY_DEFAULT':'40','CONFIG_MBEDTLS_USER_CONFIG_ENABLE':'ON','CONFIG_MBEDTLS_USER_CONFIG_FILE':'mt_pki_psa_threading_config.h','CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE':'mt_pki_psa_threading_config.h'}
 gateway_cases=[('selected',{},True),('missing-multithreading',{'CONFIG_MULTITHREADING':'OFF'},False),('late-init',{'CONFIG_KERNEL_INIT_PRIORITY_DEFAULT':'39'},False),('wrong-zephyr-config',{'CONFIG_MBEDTLS_USER_CONFIG_FILE':'other.h'},False),('wrong-tf-psa-config',{'CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE':'other.h'},False)]
 for name,changes,expected in gateway_cases:
  settings={**gateway_base,**changes};script=out/('wifi-threading-'+name+'.cmake')
  stubs='\n'.join('function('+n+')\nendfunction()'for n in ['target_include_directories','target_sources','target_link_options','set_source_files_properties','zephyr_include_directories'])
  script.write_text('\n'.join('set('+n+' "'+v+'")'for n,v in settings.items())+'\n'+stubs+'\ninclude("'+str(app/'pki-groundwork.cmake')+'")\n')
  cmd=['cmake','-P',str(script)];r=subprocess.run(cmd,capture_output=True,text=True,timeout=15);(out/('wifi-threading-'+name+'.log')).write_text(r.stdout+r.stderr);assert (r.returncode==0)==expected,(name,r.stderr);rows.append({'case':'WiFi-threading-'+name,'command':cmd,'returncode':r.returncode,'expected_success':expected,'pki_runtime':False})
 # Execute the actual native-store preprocessor guard without requiring Zephyr.
 text=(app/'src/BleTransport.cpp').read_text();start=text.index('#if defined(CONFIG_MT_BLE_NATIVE_STORE)\n#if !defined(CONFIG_BT_SETTINGS)');end=text.index('\nnamespace mt::ble {',start)
 guard=out/'ble-guard.cpp';guard.write_text(text[start:end]+'\nint fixture;\n')
 flags=['-DCONFIG_MT_BLE_NATIVE_STORE=1','-DCONFIG_BT_SETTINGS=1','-DCONFIG_BT_BONDABLE_PER_CONNECTION=1','-DCONFIG_BT_ID_MAX=1']
 for name,extra,expected in [('ordinary',[],True),('unqualified-LTO',['-DCONFIG_LTO=1'],False),('explicit-LTO',['-DCONFIG_LTO=1','-DCONFIG_MT_LTO_RUNTIME=1'],True),('lazy-CCC',['-DCONFIG_LTO=1','-DCONFIG_MT_LTO_RUNTIME=1','-DCONFIG_BT_SETTINGS_CCC_LAZY_LOADING=1'],False)]:
  cmd=['c++','-std=c++17',*flags,*extra,'-fsyntax-only',str(guard)];r=subprocess.run(cmd,capture_output=True,text=True,timeout=15);(out/('ble-'+name+'.log')).write_text(r.stdout+r.stderr);assert (r.returncode==0)==expected,(name,r.stderr);rows.append({'case':'BLE-'+name,'command':cmd,'returncode':r.returncode,'expected_success':expected})
 assert len(rows)==27 and all(sha((root/n).read_bytes())==h for n,h in before.items());result.update(status='PASS',source_stable=True)
finally:(out/'results.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
print(json.dumps({'status':result['status'],'actual_controls':len(rows),'hardware':False,'target_build':False}))
