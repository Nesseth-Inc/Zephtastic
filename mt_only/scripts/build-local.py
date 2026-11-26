#!/usr/bin/env python3
"""Compile only. Never opens a serial port or invokes a flash runner."""
import argparse,hashlib,json,os,re,shlex,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('target',choices=['xiao','t096']);p.add_argument('--zephyr',required=True,type=Path);p.add_argument('--modules',required=True,type=Path);p.add_argument('--nordic-modules',type=Path);p.add_argument('--sdk',required=True,type=Path);p.add_argument('--build',required=True,type=Path)
mode=p.add_mutually_exclusive_group();mode.add_argument('--one-packet-tx',action='store_true');mode.add_argument('--experimental-messaging-tx',action='store_true',help='Legacy alias for an unflashed XIAO normal-TX candidate')
mode.add_argument('--t096-owner-tx',action='store_true',help='Legacy alias for the owner-authorized T096 normal-TX profile; never flashes')
mode.add_argument('--normal-tx',action='store_true',help='Normal-use TX-enabled companion profile; compile only, never flashes')
mode.add_argument('--rx-only',action='store_true',help='Explicit RX-only companion boundary; compile only, never flashes')
p.add_argument('--xiao-signed-ota',metavar='MCUBOOT_MODULE',type=Path,help='Build the ordinary WiFi companion for the fresh-install signed-slot layout')
p.add_argument('--ota-public-key',type=Path,help='XIAO release signing public P-256 PEM')
p.add_argument('--pki-groundwork',action='store_true',help='Compile/link inactive PKI backend/session; no credentials, runtime support or flash')
p.add_argument('--target-private-runtime',action='store_true',help='Unqualified load-only private target composition; no provisioning, keys or flash')
p.add_argument('--store-forward-server',action='store_true',help='Opt-in XIAO PSRAM StoreForward/private WiFi TX profile; compile only, never flashes')
p.add_argument('--stock-pki-trial',action='store_true',help='T096 bounded multi-peer PKI qualification; explicit USB administration, compile only')
p.add_argument('--lto-runtime',action='store_true',help='Explicit T096 stock-PKI partial-LTO profile; requires final image/runtime qualification, never flashes')
p.add_argument('--t096-device-reset',action='store_true',
 help='Use the fresh T096 reset-floor layout only on a new board with the original LittleFS volume completely blank')
p.add_argument('--xiao-expanded-contacts',action='store_true',help='Signed full XIAO with a dedicated 198-key contact partition; requires local activation')
p.add_argument('--xiao-full-companion',action='store_true',help='Extended XIAO companion with BLE observation, private WiFi/MQTT, StoreForward, optional modules and persistent files')
p.add_argument('--t096-paged-companion',action='store_true',help='Separate 732 KiB fresh-install T096 layout with larger per-kind peer history')
p.add_argument('--t096-full-companion',action='store_true',help='Separate 728 KiB T096 fresh-install layout with extended modules, private NodeDB, public files and device reset')
p.add_argument('--t096-fresh-domain',action='store_true',
               help='Full T096 build for fresh-domain storage; application overlaps the legacy PKI flash history')
p.add_argument('--runtime-resources',action='store_true',help='Distinct USB runtime measurement image; never flashes')
p.add_argument('--ble-diagnostics',action='store_true',help='Opt-in RAM-only pairing observer; distinct -d firmware version, no security/RF changes')
p.add_argument('--ble-out-queue-depth',type=int,choices=(8,32,64),
               help='BLE outbound packet queue slots (default 8 on T096, 64 on XIAO)')
p.add_argument('--wifi-tcp',action='store_true',help='Explicit XIAO ZephCore WiFi/Meshtastic TCP build; disabled policy defaults, compile only')
p.add_argument('--wifi-mqtt',action='store_true',help='Explicit publish-only MQTT/TLS gateway overlay; requires --wifi-tcp and xiao')
p.add_argument('--source-date-epoch',type=int,help='Release source timestamp; required outside a Git checkout unless SOURCE_DATE_EPOCH is set')
p.add_argument('--jobs',type=int,default=4,help='Parallel compile jobs (1-4; default 4)')
p.add_argument('--extra-kconfig',action='append',type=Path,default=[],help='Additional board/accessory Kconfig fragment, after the selected build profile')
p.add_argument('--extra-dtc-overlay',action='append',type=Path,default=[],help='Additional board wiring overlay, applied after the selected profile')
p.add_argument('--public-files',action='store_true',help='Expose the dedicated /public application filesystem over local PhoneAPI')
p.add_argument('--paged-satellites',action='store_true',help='Use the expanded owner-bound public report archive on the private volume')
p.add_argument('--satellite-history',action='store_true',help='Use the separate private LittleFS volume for cold boot peer observations')
p.add_argument('--initialize-erased-satellite-history',action='store_true',help='Allow first-use initialization only after the entire private history partition reads erased')
p.add_argument('--stock-optional-modules',action='store_true',help='Compile the opt-in upstream module examples and local host/bench adapters')
p.add_argument('--initial-release',action='store_true',help='Opt-in initial companion/repeater build; compiles TAK_TRACKER out and verifies resolved Kconfig')
p.add_argument('--xiao-l76k-managed',action='store_true',help='Probe the declared Seeed L76K baud and configure its normal receiver output')
p.add_argument('--xiao-l76k',action='store_true',help='Use an explicitly fitted XIAO L76K with the radio-conflicting expansion RESET disconnected')
p.add_argument('--xiao-expansion-oled',action='store_true',help='Use the 128x64 SSD1306 on the XIAO expansion board')
p.add_argument('--mqtt-ca-der',type=Path,help='Replace the MQTT ISRG Root X1 with one public DER root for this deployment')
a=p.parse_args()
if a.xiao_l76k_managed:
 if a.xiao_l76k:p.error('Select one XIAO L76K profile')
 a.xiao_l76k=True
if a.xiao_l76k and a.target!='xiao':p.error('--xiao-l76k requires the XIAO')
if a.xiao_expansion_oled and a.target!='xiao':p.error('--xiao-expansion-oled requires the XIAO')
if a.xiao_expanded_contacts and not (a.target=='xiao' and a.xiao_full_companion and a.xiao_signed_ota):
 p.error('--xiao-expanded-contacts requires --xiao-full-companion and --xiao-signed-ota on XIAO')
if a.xiao_expanded_contacts:
 a.paged_satellites=True
if a.xiao_full_companion:
 if a.target!='xiao' or a.initial_release:
  p.error('--xiao-full-companion selects the extended XIAO firmware')
 a.target_private_runtime=a.wifi_tcp=a.wifi_mqtt=a.store_forward_server=True
 a.stock_optional_modules=a.satellite_history=a.public_files=True
if a.t096_paged_companion:
 a.t096_full_companion=a.paged_satellites=True
if a.t096_full_companion:
 if a.target!='t096' or a.initial_release:
  p.error('--t096-full-companion selects the extended T096 firmware')
 a.normal_tx=a.stock_pki_trial=a.lto_runtime=a.t096_fresh_domain=True
 a.t096_device_reset=a.stock_optional_modules=a.satellite_history=a.public_files=True
if a.paged_satellites and not a.satellite_history:p.error('--paged-satellites requires --satellite-history')
if a.initialize_erased_satellite_history and not a.satellite_history:p.error('--initialize-erased-satellite-history requires --satellite-history')
if a.satellite_history and a.target=='t096' and not a.t096_fresh_domain:p.error('T096 private history requires the fresh-domain layout')
if a.satellite_history and not (a.target_private_runtime or a.stock_pki_trial):p.error('--satellite-history requires a profile with the identity-bound private runtime')
if a.ble_out_queue_depth is None:
 a.ble_out_queue_depth=8 if a.target=='t096' else 64
if not 1<=a.jobs<=4:p.error('--jobs must be between 1 and 4')
if (a.one_packet_tx or a.experimental_messaging_tx) and a.target!='xiao':p.error('Use the explicit --t096-owner-tx profile for T096')
if a.t096_owner_tx and a.target!='t096':p.error('--t096-owner-tx requires t096')
if a.normal_tx and a.target not in ('xiao','t096'):p.error('--normal-tx requires a companion target')
if a.target_private_runtime and a.one_packet_tx:p.error('Private runtime requires the normal endpoint')
if a.target_private_runtime and (a.normal_tx or a.experimental_messaging_tx or a.t096_owner_tx):p.error('Private runtime remains load-only and cannot use a normal-TX profile')
if a.store_forward_server and (a.target!='xiao' or not a.target_private_runtime or not a.wifi_tcp or not a.wifi_mqtt or a.one_packet_tx or a.rx_only or a.normal_tx or a.experimental_messaging_tx or a.t096_owner_tx or a.runtime_resources or a.ble_diagnostics):p.error('--store-forward-server requires XIAO --target-private-runtime --wifi-tcp --wifi-mqtt without other TX/resource/diagnostic profiles')
if a.stock_pki_trial and (a.target!='t096' or not a.normal_tx or a.target_private_runtime or a.runtime_resources):p.error('--stock-pki-trial requires T096 --normal-tx without resource/private profiles')
if a.lto_runtime and (a.target!='t096' or not a.stock_pki_trial):p.error('--lto-runtime requires T096 --normal-tx --stock-pki-trial')
if a.wifi_mqtt and not a.wifi_tcp:p.error('--wifi-mqtt requires --wifi-tcp')
if a.wifi_mqtt and a.target!='xiao':p.error('--wifi-mqtt requires the WiFi-capable xiao target')
if a.wifi_tcp and a.target!='xiao':p.error('--wifi-tcp requires the WiFi-capable xiao target')
if a.t096_device_reset and not a.t096_fresh_domain:p.error('--t096-device-reset requires --t096-fresh-domain')
if a.t096_fresh_domain and not (a.target=='t096' and a.stock_pki_trial and a.normal_tx and a.lto_runtime):
 p.error('--t096-fresh-domain requires t096 --normal-tx --stock-pki-trial --lto-runtime')
if a.initial_release:
 if a.target=='t096' and (a.runtime_resources or a.ble_diagnostics):
  p.error('--initial-release does not combine with runtime-resource or BLE-diagnostic profiles')
 if a.target=='t096' and not (a.normal_tx and a.stock_pki_trial and a.lto_runtime):
  p.error('--initial-release on t096 requires --normal-tx --stock-pki-trial --lto-runtime')
 if a.target=='xiao' and (a.runtime_resources or a.ble_diagnostics):
  p.error('--initial-release does not combine with runtime-resource or BLE-diagnostic profiles')
 if a.target=='xiao' and not (a.target_private_runtime and a.wifi_tcp and a.wifi_mqtt):
  p.error('--initial-release on xiao requires --target-private-runtime --wifi-tcp --wifi-mqtt')
app=Path(__file__).resolve().parents[1];out=a.build.resolve();out.mkdir(parents=True,exist_ok=True)
modules=[a.modules/x for x in ['crypto/mbedtls','crypto/tf-psa-crypto','lib/loramac-node']]
if a.satellite_history or a.public_files:modules.append(a.modules/'fs/littlefs')
if a.xiao_signed_ota:
 if a.target!='xiao' or not a.wifi_tcp:p.error('--xiao-signed-ota requires XIAO --wifi-tcp')
 modules.append(a.xiao_signed_ota.resolve(strict=True))
if a.target=='xiao':modules += [a.modules/x for x in ['hal/espressif','hal/xtensa']]
else:
 if not a.nordic_modules:p.error('--nordic-modules required for t096')
 modules += [a.nordic_modules/x for x in ['hal/cmsis','hal/cmsis_6','hal/nordic']]
epoch=a.source_date_epoch
if epoch is None and os.environ.get('SOURCE_DATE_EPOCH'):epoch=int(os.environ['SOURCE_DATE_EPOCH'])
if epoch is None:
 if not (app.parent/'.git').exists():p.error('Pass --source-date-epoch for an exported source tree')
 try:epoch=int(subprocess.check_output(['git','-C',str(app),'log','-1','--format=%ct'],text=True).strip())
 except (subprocess.CalledProcessError,ValueError):p.error('Pass --source-date-epoch for an exported source tree')
if epoch<0:p.error('--source-date-epoch must be nonnegative')
env={**os.environ,'ZEPHYR_BASE':str(a.zephyr.resolve()),'ZEPHYR_TOOLCHAIN_VARIANT':'zephyr','ZEPHYR_SDK_INSTALL_DIR':str(a.sdk.resolve()),'SOURCE_DATE_EPOCH':str(epoch),'PYTHONDONTWRITEBYTECODE':'1'}
for name in ['CC','CXX','CPP','AS','LD','AR','NM','OBJCOPY','OBJDUMP','RANLIB','READELF','STRIP',
             'CROSS_COMPILE','TOOLCHAIN_HOME','GNUARMEMB_TOOLCHAIN_PATH','ESPRESSIF_TOOLCHAIN_PATH',
             'CMAKE_TOOLCHAIN_FILE','SDKROOT','CFLAGS','CXXFLAGS','ASMFLAGS','CPPFLAGS','LDFLAGS']:
 env.pop(name,None)
maps={str(app.parent):'/src/zt',str(a.zephyr.resolve()):'/src/deps/zephyr',str(a.modules.resolve()):'/src/deps/modules',str(a.sdk.resolve()):'/src/toolchain',str(out):'/src/build/'+a.target}
if a.nordic_modules:maps[str(a.nordic_modules.resolve())]='/src/deps/modules'
flags=[]
for old,new in maps.items():flags+=['-fmacro-prefix-map='+old+'='+new,'-fdebug-prefix-map='+old+'='+new]
flags+=['-fno-record-gcc-switches','-gno-record-gcc-switches']
mapped_flags=shlex.join(flags)
cxx_flags=[*flags,'-DMT_BLE_OUT_QUEUE_DEPTH='+str(a.ble_out_queue_depth)]
mapped_cxx_flags=shlex.join(cxx_flags)
# Preserve the virtual-environment entry point. Resolving its Python symlink
# selects the base interpreter and silently drops the staged site-packages.
cmd=['cmake','-S',str(app),'-B',str(out),'-G','Ninja','-DPython3_EXECUTABLE='+sys.executable,'-DBOARD='+('xiao_esp32s3/esp32s3/procpu' if a.target=='xiao' else 'heltec_t096/nrf52840'),'-DZEPHYR_MODULES='+';'.join(str(x.resolve()) for x in modules),'-DEXTRA_CONF_FILE='+str(app/'boards'/f'{a.target}.conf'),'-DDTC_OVERLAY_FILE='+str(app/'boards'/f'{a.target}.overlay')]
overlays=[]
if a.target=='xiao' and a.wifi_tcp:
 overlays=[app/'boards/xiao.conf',app/'boards/xiao-wifi-tcp.conf']
 if a.wifi_mqtt:overlays.append(app/'boards/xiao-wifi-mqtt.conf')
 if a.store_forward_server:overlays.append(app/'boards/xiao-store-forward.conf')
elif a.initial_release or a.t096_fresh_domain:
 overlays=[app/'boards'/f'{a.target}.conf']
if a.initial_release:overlays.append(app/'boards/initial-release.conf')
if a.t096_fresh_domain:
 overlays.append(app/'boards/t096-fresh-domain.conf')
 if not a.initial_release:overlays.append(app/'boards/t096-onboard.conf')
 # Zephyr applies the additional overlay after the regular T096 board overlay.
 cmd+=['-DEXTRA_DTC_OVERLAY_FILE='+str(app/'boards/t096-fresh-domain.overlay'),
       '-DMT_LTO_CANARY_NO_PARTIAL_INLINING=ON']
extra_dtc=[f.resolve(strict=True) for f in a.extra_dtc_overlay]
expansion_oled=app/'boards/xiao-expansion-oled.overlay'
if a.xiao_signed_ota:
 if not a.ota_public_key:p.error('--ota-public-key is required for signed XIAO OTA')
 cmd+=['-DMT_OTA_PUBLIC_KEY='+str(a.ota_public_key.resolve(strict=True))]
 extra_dtc.append(app/'boards/xiao-signed-ota.overlay')
 overlays.append(app/'boards/xiao-signed-ota.conf')
if a.xiao_expanded_contacts:extra_dtc.append(app/'boards/xiao-expanded-contacts.overlay')
if a.xiao_l76k:extra_dtc.append(app/'boards/xiao-l76k.overlay')
if a.xiao_expansion_oled and expansion_oled not in extra_dtc:extra_dtc.append(expansion_oled)
if a.t096_device_reset:extra_dtc.append(app/'boards/t096-device-reset.overlay')
if a.satellite_history:extra_dtc.append(app/'boards'/f'{a.target}-satellite-history.overlay')
if a.paged_satellites and a.target=='t096':extra_dtc.append(app/'boards/t096-paged-satellites.overlay')
if a.t096_full_companion:
 layout='t096-paged-companion.overlay' if a.t096_paged_companion else 't096-full-companion.overlay'
 extra_dtc.append(app/'boards'/layout)
 if a.t096_paged_companion:cmd+=['-DCONFIG_MT_T096_PAGED_LAYOUT=y']
 cmd+=['-DCONFIG_MT_T096_FULL_COMPANION=y']
if extra_dtc:
 if a.t096_fresh_domain:extra_dtc.insert(0,app/'boards/t096-fresh-domain.overlay')
 cmd=[v for v in cmd if not v.startswith('-DEXTRA_DTC_OVERLAY_FILE=')]
 cmd.append('-DEXTRA_DTC_OVERLAY_FILE='+';'.join(map(str,extra_dtc)))
optional_profiles=[]
if a.stock_optional_modules:
 if a.one_packet_tx:p.error('--stock-optional-modules requires the normal companion endpoint')
 if not overlays:overlays=[app/'boards'/f'{a.target}.conf']
 optional_profiles=[app/'boards/stock-optional-modules.conf']
 if a.target=='t096' or (a.target=='xiao' and expansion_oled in extra_dtc):
  optional_profiles.append(app/'boards/t096-stock-ui.conf')
 overlays.extend(optional_profiles)
if a.satellite_history:
 if not overlays:overlays=[app/'boards'/f'{a.target}.conf']
 overlays.append(app/'boards/satellite-history.conf')
 if a.paged_satellites:
  cmd+=['-DCONFIG_MT_PAGED_SATELLITES=y']
  if a.target=='t096':cmd+=['-DCONFIG_FS_LITTLEFS_CACHE_SIZE=128']
 if a.initialize_erased_satellite_history:cmd+=['-DCONFIG_MT_SATELLITE_INITIALIZE_ERASED=y']
if a.public_files:
 if not overlays:overlays=[app/'boards'/f'{a.target}.conf']
 overlays.append(app/'boards/public-files.conf')
 if a.satellite_history:overlays.append(app/'boards/public-history-files.conf')
if a.xiao_full_companion:
 onboard=app/'boards/xiao-onboard.conf'
 optional_profiles.append(onboard)
 overlays.append(onboard)
if a.xiao_expanded_contacts:
 contacts=app/'boards/xiao-expanded-contacts.conf'
 overlays.append(contacts)
 optional_profiles.append(contacts)
extra_kconfig=[f.resolve(strict=True) for f in a.extra_kconfig]
if a.xiao_l76k:
 gnss_profile=app/'boards/xiao-l76k.conf'
 extra_kconfig.append(gnss_profile)
 optional_profiles.append(gnss_profile)
 if a.xiao_l76k_managed:
  managed=app/'boards/xiao-l76k-managed.conf'
  extra_kconfig.append(managed)
  optional_profiles.append(managed)
if extra_kconfig:
 if not overlays:overlays=[app/'boards'/f'{a.target}.conf']
 overlays.extend(extra_kconfig)
if overlays:
 cmd=[x if not x.startswith('-DEXTRA_CONF_FILE=')else '-DEXTRA_CONF_FILE='+';'.join(map(str,overlays))for x in cmd]
normal_tx=a.normal_tx or a.experimental_messaging_tx or a.t096_owner_tx or a.store_forward_server
base_profile='store-forward-server' if a.store_forward_server else 'one-packet-tx' if a.one_packet_tx else 'normal-tx' if normal_tx else 'rx-only'
profile=base_profile
if a.xiao_full_companion:profile+='-extended-onboard'
if a.xiao_expanded_contacts:profile+='-198-contacts'
if a.xiao_l76k:profile+='-l76k-managed' if a.xiao_l76k_managed else '-l76k'
if a.t096_full_companion:profile+='-732KiB-new-layout' if a.t096_paged_companion else '-728KiB-new-layout'
if a.public_files:profile+='-public-files'
if a.stock_optional_modules:profile+='-stock-optional'
if a.satellite_history:profile+='-satellite-history'
if a.paged_satellites:profile+='-paged'
if a.initial_release:profile+='-initial-release'
if a.t096_fresh_domain:profile+='-fresh-domain' if a.initial_release else '-fresh-domain-onboard'
if a.t096_device_reset:profile+='-device-reset'
cmd+=['-DCONFIG_MT_ALLOW_TX='+('y' if a.one_packet_tx or normal_tx else 'n'),'-DCONFIG_MT_NORMAL_ENDPOINT='+('n' if a.one_packet_tx else 'y'),'-DCONFIG_MT_T096_OWNER_TX='+('y' if a.target=='t096' and normal_tx else 'n'),'-DCONFIG_MT_NORMAL_TX_DEFAULT='+('y' if normal_tx else 'n')]
cmd+=['-DCONFIG_MT_PKI_GROUNDWORK='+('y' if a.pki_groundwork or a.target_private_runtime or a.stock_pki_trial else 'n')]
cmd+=['-DCONFIG_MT_PKI_TARGET_RUNTIME='+('y' if a.target_private_runtime or a.stock_pki_trial else 'n')]
cmd+=['-DCONFIG_MT_STORE_FORWARD_PSRAM='+('y' if a.store_forward_server else 'n')]
cmd+=['-DCONFIG_MT_PKI_STOCK_TRIAL='+('y' if a.stock_pki_trial else 'n')]
cmd+=['-DCONFIG_MT_LTO_RUNTIME='+('y' if a.lto_runtime else 'n')]
if a.lto_runtime:
 # Zephyr also passes its size choice to the final GCC LTO link. The Nordic
 # controller's reviewed timing-critical translation units still override to O2.
 cmd+=['-DCONFIG_LTO=y','-DCONFIG_ISR_TABLES_LOCAL_DECLARATION=y',
       '-DCONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE=y']
# Private submission keeps companion/protobuf frames live over durable NVS writes.
# M69's larger journal and peer administration retain more bounded scratch
# over storage calls. Ordinary non-PKI profiles keep their existing stack size.
if a.stock_pki_trial:cmd+=['-DCONFIG_MAIN_STACK_SIZE=16384']
if a.mqtt_ca_der:
 if not a.wifi_mqtt or not a.mqtt_ca_der.is_file():p.error('--mqtt-ca-der requires XIAO MQTT and a public DER CA file')
 cmd+=['-DMT_MQTT_CA_DER='+str(a.mqtt_ca_der.resolve())]
cmd+=['-DCONFIG_MT_RUNTIME_RESOURCES='+('y' if a.runtime_resources else 'n')]
cmd+=['-DCONFIG_MT_BLE_DIAGNOSTICS='+('y' if a.ble_diagnostics else 'n')]
cmd+=['-DEXTRA_CFLAGS='+mapped_flags,'-DEXTRA_CXXFLAGS='+mapped_cxx_flags,'-DEXTRA_AFLAGS='+mapped_flags]
commandFile=out/'configure-command.json';cache=out/'CMakeCache.txt'
if commandFile.exists():
 try:previous=json.loads(commandFile.read_text())
 except (OSError,json.JSONDecodeError):p.error('Existing configure-command.json is unreadable; use a fresh profile-owned build directory')
 if previous!=cmd:p.error('Build directory belongs to a different configure profile; use a fresh profile-owned build directory')
elif cache.exists():p.error('Existing CMake cache has no matching configure receipt; use a fresh profile-owned build directory')
provenance={'source_date_epoch':epoch,'prefix_maps':maps,'compiler_flags':flags,'hardware_actions':False,
 'cxx_compiler_flags':cxx_flags,
 'wifi_tcp':a.wifi_tcp,'wifi_mqtt':a.wifi_mqtt,
 'ble_diagnostics':a.ble_diagnostics,'pki_groundwork':a.pki_groundwork or a.target_private_runtime or a.stock_pki_trial,
 'ble_out_queue_depth':a.ble_out_queue_depth,
 'stock_pki_trial':a.stock_pki_trial,'store_forward_server':a.store_forward_server,
 'lto_runtime':a.lto_runtime,
 'target_private_runtime':a.target_private_runtime or a.stock_pki_trial,'profile':profile,'normal_tx':normal_tx,
 'initial_release':a.initial_release,'xiao_full_companion':a.xiao_full_companion,
 'tak_tracker_expected':'n' if a.initial_release else None,
 'extra_kconfig':[{str(f):hashlib.sha256(f.read_bytes()).hexdigest()} for f in extra_kconfig],
 'extra_dtc_overlays':[{str(f):hashlib.sha256(f.read_bytes()).hexdigest()} for f in extra_dtc],
 'stock_optional_configs':{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in optional_profiles},
 'initial_release_overlay':str(app/'boards/initial-release.conf') if a.initial_release else None,
 'initial_release_overlay_sha256':hashlib.sha256((app/'boards/initial-release.conf').read_bytes()).hexdigest() if a.initial_release else None,
 'one_packet_tx':a.one_packet_tx,'rx_only':base_profile=='rx-only',
 'jobs':a.jobs,'configure_status':'NOT_RUN','build_status':'NOT_RUN'}
settings_inputs={}
for name in ('subsys/settings/src/settings.c','subsys/settings/src/settings_store.c',
             'subsys/settings/src/settings_nvs.c','subsys/bluetooth/host/settings.c',
             'subsys/bluetooth/host/keys.c'):
 actual=(a.zephyr/name).read_bytes();expected=(app/'dependency-overlay/zephyr'/name).read_bytes()
 if actual!=expected:p.error('Native Settings SDK body differs from pinned source overlay: '+name)
 settings_inputs[name]={'bytes':len(actual),'sha256':hashlib.sha256(actual).hexdigest()}
provenance['native_settings_inputs']=settings_inputs
def save(): (out/'build-provenance-private.json').write_text(json.dumps(provenance,indent=2)+'\n')
def run(command,log):
 with (out/log).open('w') as f:result=subprocess.run(command,env=env,stdout=f,stderr=subprocess.STDOUT)
 print(log,result.returncode,flush=True);return result.returncode
def compiler(language):
 name='CMAKE_'+language+'_COMPILER';value=None
 if cache.exists():
  match=re.search(r'^'+name+r'(?::[^=]+)?=(.+)$',cache.read_text(),re.M)
  if match:value=match.group(1).strip()
 if not value:
  for record in out.rglob('CMake'+language+'Compiler.cmake'):
   match=re.search(r'set\('+name+r'\s+"([^"]+)"\)',record.read_text())
   if match:value=match.group(1);break
 if not value:raise RuntimeError('Configured build did not record '+name)
 path=Path(value).resolve(strict=True);sdk=a.sdk.resolve()
 if not path.is_relative_to(sdk):raise RuntimeError(name+' resolved outside supplied SDK: '+str(path))
 version=subprocess.run([str(path),'--version'],capture_output=True,text=True,check=True,env=env).stdout.splitlines()
 return {'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'version':version[0] if version else ''}
commandFile.write_text(json.dumps(cmd,indent=2)+'\n');save()
rc=run(cmd,'configure.log')
if rc:
 provenance['configure_status']='FAIL';save();print((out/'configure.log').read_text()[-6500:]);raise SystemExit(rc)
try:provenance['resolved_compilers']={'C':compiler('C'),'CXX':compiler('CXX')}
except (OSError,RuntimeError,subprocess.CalledProcessError) as error:
 provenance['configure_status']='FAIL_ATTESTATION';provenance['compiler_error']=str(error);save();raise SystemExit(str(error))
provenance['configure_status']='PASS';save()
if a.initial_release:
 config=out/'zephyr/.config'
 try:resolved=config.read_text()
 except OSError as error:
  provenance['configure_status']='FAIL_RELEASE_CONFIG';provenance['release_config_error']=str(error);save()
  raise SystemExit('initial-release resolved Zephyr .config is unavailable')
 match=re.search(r'^CONFIG_MT_TAK_TRACKER=(.+)$',resolved,re.M)
 resolved_tak='y' if match and match.group(1).strip()=='y' else 'n' if re.search(r'^# CONFIG_MT_TAK_TRACKER is not set$',resolved,re.M) or (match and match.group(1).strip()=='n') else 'missing-or-unrecognized'
 provenance['tak_tracker_resolved']=resolved_tak
 if resolved_tak!='n':
  provenance['configure_status']='FAIL_RELEASE_CONFIG'
  provenance['release_config_error']='CONFIG_MT_TAK_TRACKER must resolve to n for --initial-release'
  save()
  raise SystemExit(provenance['release_config_error'])
 save()
if optional_profiles:
 resolved=set((out/'zephyr/.config').read_text().splitlines())
 required={line for f in optional_profiles for line in f.read_text().splitlines() if line.endswith('=y')}
 if not required<=resolved:
  provenance['configure_status']='FAIL_OPTIONAL_CONFIG';save()
  raise SystemExit('Missing opt-in stock modules: '+', '.join(sorted(required-resolved)))
if a.t096_fresh_domain:
 resolved=(out/'zephyr/.config').read_text().splitlines()
 expected=['MT_PKI_FRESH_DOMAIN_ONLY','MT_RF_ADMIN_GET','MT_NORMAL_TX_DEFAULT','MT_DISPLAY','MT_GNSS_T096']
 if not a.initial_release:expected+=['MT_TAK_TRACKER','MT_GPIO_MODULES','MT_PAXCOUNTER','BT_OBSERVER']
 missing=[sym for sym in expected if 'CONFIG_'+sym+'=y' not in resolved]
 if missing:
  provenance['configure_status']='FAIL_PROFILE_CONFIG';save()
  raise SystemExit('T096 fresh-domain profile is missing '+', '.join(missing))
rc=run(['cmake','--build',str(out),'--parallel',str(a.jobs)],'build.log')
provenance['build_status']='PASS' if not rc else 'FAIL';save()
if rc:print((out/'build.log').read_text()[-6500:]);raise SystemExit(rc)
