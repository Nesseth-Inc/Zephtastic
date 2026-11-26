#!/usr/bin/env python3
"""Run the production BLE updater and ordinary BLE security callbacks with inert hardware/storage."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--short-enums',action='store_true')
p.add_argument('--stock-loader',type=Path);p.add_argument('--android',type=Path);a=p.parse_args()
here=Path(__file__).resolve().parent;app=here.parents[1];root=app.parent;out=a.output.resolve()
sys.path.insert(0,str(root));import host_test_toolchain as host
out.mkdir(parents=True,exist_ok=False);(out/'update').mkdir()
for name in ('SignedBleOta.cpp','SignedBleOta.h','BleOtaStream.h'):
    shutil.copyfile(app/'src/update'/name,out/'update'/name)
(out/'BleTransport.h').symlink_to(app/'src/BleTransport.h')
shutil.copyfile(here/'SignedOta.h',out/'update/SignedOta.h')
for name in ('kernel.h','bluetooth/bluetooth.h','bluetooth/conn.h','bluetooth/gatt.h','settings/settings.h','sys/atomic.h','sys/byteorder.h'):
    h=out/'zephyr'/name;h.parent.mkdir(parents=True,exist_ok=True);h.write_text('#include "ZephyrOtaMock.h"\n')
oracle=[]
if a.stock_loader:
    processor=(a.stock_loader/'src/ota_processor.cpp').read_bytes()
    assert hashlib.sha256(processor).hexdigest()=='eae9be2fadd1f1a9b60e5ceddf6f871235b1ae5fa04263883460dde2c112f0f1'
    original=processor.decode()
    start=original.index('void OtaProcessor::process(');end=original.index('void OtaProcessor::handleCommand()',start)
    (out/'stock-process.inc').write_text(original[start:end])
    oracle=['-DTEST_STOCK_PROCESSOR=1']
    src=(a.stock_loader/'src/ble_ota.cpp').read_bytes()
    assert hashlib.sha256(src).hexdigest()=='57ec1c6f40779398f236eaa1998f2dcc3e965788d00fcbacaecefb6dd9defafb'
    assert b'updateConnParams(connInfo.getConnHandle(), 12, 12, 0, 400)' in src
    carrier=(app/'src/update/SignedBleOta.cpp').read_text()
    for uuid in ('4fafc201-1fb5-459e-8fcc-c5c9c331914b','62ec0272-3ec5-11eb-b378-0242ac130003','62ec0272-3ec5-11eb-b378-0242ac130005'):
        assert uuid in src.decode().lower() and ','.join('0x'+part for part in uuid.split('-')) in carrier
if a.android:
    path=a.android/'feature/firmware/src/commonMain/kotlin/org/meshtastic/feature/firmware/ota/BleOtaTransport.kt'
    assert hashlib.sha256(path.read_bytes()).hexdigest()=='cd865d4c91b28d0c8fff26206dcb0de1d89c5d9d50746e95767cca5e76c8c1dd'
flags=[*oracle,'-std=c++17','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-g',
 '-DCONFIG_MT_SIGNED_BLE_OTA=1','-DCONFIG_MT_BLE_LOG_RADIO=1','-DCONFIG_MT_DISPLAY=1','-DCONFIG_BT_APP_PASSKEY=1',
 '-I'+str(out),'-I'+str(here),'-I'+str(app/'tests'),'-I'+str(app/'src')]
if a.short_enums:flags+=['-fshort-enums']
subprocess.run([*host.cxx(),*flags,here/'cases.cpp','-o',out/'cases'],check=True)
cases=('android-key-retry','disabled','success','params-refused','first-write','recycle','recycle-busy','wrong-advertiser','reconnect','budget','wronghash','signature','signature-async','signature-full-async','syntax-async','terminal-peer-lag','terminal-local-timeout','terminal-disconnect','terminal-timeout','overflow','disconnect','downgrade','timeout','timed-mtu23','timed-idle','timed-lifetime','timed-preamble')
for case in cases:
    subprocess.run([out/'cases',case],check=True,timeout=10,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
native_cases=('android-key-retry','lease-first','lease-rx','lease-ack','lease-disconnect','lease-failure','lease-stale','lease-late')
native_flags=['-DCONFIG_MT_BLE_NATIVE_STORE=1','-DCONFIG_BT_SETTINGS=1','-DCONFIG_BT_BONDABLE_PER_CONNECTION=1',
              '-DCONFIG_BT_MAX_PAIRED=5','-DCONFIG_BT_ID_MAX=1']
subprocess.run([*host.cxx(),*flags,*native_flags,here/'cases.cpp',app/'src/BleSettingsAdmission.cpp','-o',out/'native-cases'],check=True)
for case in native_cases:
    subprocess.run([out/'native-cases',case],check=True,timeout=10,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'results.json').write_text(json.dumps({'status':'PASS','cases':cases,'native_settings_cases':native_cases,'hardware':False,'flash_backend':'test double',
    'stock_parser_compared':bool(oracle),'android_source_checked':bool(a.android),'short_enums':a.short_enums,
    'sha256':{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in (app/'src/BleTransport.cpp',app/'src/update/SignedBleOta.cpp',app/'src/update/BleOtaStream.h')}},indent=2)+'\n')
