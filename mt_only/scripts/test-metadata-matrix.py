#!/usr/bin/env python3
"""Real protobuf metadata and capability regression across TX/diagnostic profiles."""
import argparse,hashlib,json,os,re,shlex,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--source',type=Path,help='Alternate repository checkout to test without modifying it');p.add_argument('--short-enums',action='store_true');a=p.parse_args()
tests=Path(__file__).resolve().parents[1];root=(a.source or tests.parent).resolve();app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
# BootCounter is generic NVS I/O. A failed admission must never reach it.
startup=(app/'src/main.cpp').read_text().split('static ApplicationStartup initializeApplication() {',1)[1]
boundaries=['privateStartup.prepare(node)', 'deviceReset::freshBinding(settings,binding)',
            'recoverRemoteDeviceAtBoot()', 'privateStartup.activate(service,backendReady)',
            'if(storageReady)myBootCount=mt::companion::incrementBootCounter(settings)',
            'mt::NormalTxStartup::activate(']
assert startup.count('incrementBootCounter(')==1
assert [startup.index(b) for b in boundaries]==sorted(startup.index(b) for b in boundaries)

proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'));host_link=[]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
source_hashes={str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in (app/'src/Version.h',app/'src/Companion.h',proto/'mesh.pb.h',proto/'admin.pb.h')}
commands=[];objects=[];profiles=[]
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(src.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
# Compile the production selector, including boards which share a protocol ID.
board_cases = {
    "ADAFRUIT_FEATHER_RFM95_RP2040": 76,
    "HELTEC_T096": 127,
    "HELTEC_T1": 133,
    "HELTEC_T114": 69,
    "HELTEC_T114_V2": 69,
    "HELTEC_WIFI_LORA32_V3": 43,
    "HELTEC_WIFI_LORA32_V4": 110,
    "HELTEC_WIFI_LORA32_V43": 110,
    "HELTEC_WIFI_LORA32_V4_R8": 132,
    "HELTEC_WIRELESS_STICK_LITE_V3": 44,
    "HELTEC_WIRELESS_TRACKER": 48,
    "HELTEC_WIRELESS_TRACKER_V2": 113,
    "LILYGO_T3S3": 16,
    "LILYGO_TECHO": 7,
    "LILYGO_TIMPULSE_PLUS": 135,
    "LILYGO_TLORA_C6": 83,
    "LORA_E5_MINI": 73,
    "M5STACK_CORES3": 80,
    "M5STACK_UNITC6L": 111,
    "MESHNOLOGY_W12": 145,
    "MESHTRACKER_X1": 128,
    "MUZIWORKS_R1NEO": 101,
    "NRF52840DONGLE": 40,
    "PROMICRO_LR2021": 63,
    "PROMICRO_SX1262": 63,
    "RAK3172": 72,
    "RAK3401_1WATT": 117,
    "RAK4631": 9,
    "RAK_WISMESH_TAG": 105,
    "RPI_PICO": 47,
    "RPI_PICO2": 79,
    "SENSECAP_SOLAR": 95,
    "STATION_G2": 31,
    "T1000_E": 71,
    "THINKNODE_M1": 89,
    "THINKNODE_M3": 115,
    "THINKNODE_M6": 120,
    "THINKNODE_M9": 131,
    "TTGO_LORA32": 3,
    "TTGO_TBEAM": 4,
    "TWATCH_S3": 51,
    "T_DECK": 50,
    "WIO_TRACKER_L1": 99,
    "WIO_TRACKER_L1_1W": 144,
    "WIO_WM1110_DEV_KIT": 21,
    "XIAO_ESP32S3": 81,
    "XIAO_NRF52840": 88,
}
def compile_probe(definitions, source, succeeds=True):
    cmd = [*cxx, '-std=c++17', *flags, *['-DCONFIG_' + x for x in definitions],
           '-x', 'c++', '-fsyntax-only', '-']
    result = subprocess.run(cmd, input='#include "BoardIdentity.h"\n#include <string_view>\n' + source,
                            text=True, capture_output=True)
    if (result.returncode == 0) != succeeds:
        raise RuntimeError(f'{definitions}: unexpected compiler result {result.returncode}\n{result.stderr}')
    if not succeeds and "Select an assigned Meshtastic hardware model" not in result.stderr:
        raise RuntimeError(f'{definitions}: invalid configuration failed for the wrong reason')

cases = [(['BOARD_' + board], model, model, {81: 'mt-zephyr-xiao', 127: 'mt-zephyr-t096'}.get(model, ''))
         for board, model in board_cases.items()]
cases += [
    ([], 0, 0, ''),
    (['BOARD_XIAO_ESP32S3', 'MT_XIAO_EXPANDED_CONTACTS'], 81, 81, 'mt-zephyr-xiao-198'),
    (['BOARD_HELTEC_T114_V2', 'MT_HARDWARE_MODEL=151'], 151, 69, ''),
    (['MT_HARDWARE_MODEL=81'], 81, 0, ''),
    (['MT_HARDWARE_MODEL=255'], 255, 0, ''),
]
for symbols, model, capabilities, target in cases:
    compile_probe(symbols, f'static_assert(mt::board::Model == {model});\n'
                  f'static_assert(mt::board::capabilityModel(mt::board::Model) == {capabilities});\n'
                  f'static_assert(mt::board::HasUpdateTarget == {str(bool(target)).lower()});\n'
                  f'static_assert(std::string_view(mt::board::firmwareTarget(mt::board::Model)) == "{target}");\n')

# Derive the expected allowed set from the pinned enum, independently of the firmware.
enums = re.findall(r'meshtastic_HardwareModel_(\w+)\s*=\s*(\d+)', (proto/'mesh.pb.h').read_text())
allowed = {int(value) for name, value in enums if name != 'UNSET' and not name.endswith('_RESERVED')}
compile_probe([], '\n'.join(f'static_assert(mt::board::supportedModel({n}) == {str(n in allowed).lower()});'
                            for n in range(256)))
for invalid in [-1, 28, 104, 152, 254, 256]:
    compile_probe([f'MT_HARDWARE_MODEL={invalid}'], '', succeeds=False)
matrix=(
 ('rx',[]),('tx',['-DCONFIG_MT_ALLOW_TX=1','-DCONFIG_MT_NORMAL_ENDPOINT=1']),
 ('rxd',['-DCONFIG_MT_BLE_DIAGNOSTICS=1']),
 ('txd',['-DCONFIG_MT_ALLOW_TX=1','-DCONFIG_MT_NORMAL_ENDPOINT=1','-DCONFIG_MT_BLE_DIAGNOSTICS=1']),
 ('one-tx',['-DCONFIG_MT_ALLOW_TX=1']),
 ('one-txd',['-DCONFIG_MT_ALLOW_TX=1','-DCONFIG_MT_BLE_DIAGNOSTICS=1']),
 ('private-rx',['-DCONFIG_MT_PKI_TARGET_RUNTIME=1']),
 ('private-rxd',['-DCONFIG_MT_PKI_TARGET_RUNTIME=1','-DCONFIG_MT_BLE_DIAGNOSTICS=1']),
)
if 'SignedVersionText' in (app/'src/Version.h').read_text():
 matrix+=(('signed',['-DCONFIG_MT_SIGNED_WIFI_OTA=1']),)
for name,extra in matrix:
 cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,*extra,str(tests/'tests/metadata_tests.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*host_link,'-o',str(out/name)]
 commands.append(cmd);built=subprocess.run(cmd,capture_output=True,text=True);(out/(name+'-build.log')).write_text(built.stdout+built.stderr)
 if built.returncode:
  profiles.append({'name':name,'status':'FAIL','compile_returncode':built.returncode});print(name,'BUILD FAIL');continue
 run=subprocess.run([str(out/name)],capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(run.stdout+run.stderr);print(name,run.stdout,run.stderr)
 profiles.append({'name':name,'status':'PASS' if run.returncode==0 else 'FAIL','returncode':run.returncode,'scenario_groups':sum(line.startswith('PASS ') for line in run.stdout.splitlines())})
passed=all(item['status']=='PASS' for item in profiles)
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'status':'PASS' if passed else 'FAIL','source':str(root),'source_hashes':source_hashes,'ASAN_UBSAN':True,'enum_abi':'short' if a.short_enums else 'default','hardware':False,'profiles':profiles,'scenario_groups':sum(item.get('scenario_groups',0) for item in profiles)},indent=2)+'\n')
raise SystemExit(0 if passed else 1)
