#!/usr/bin/env python3
"""ASAN/UBSAN companion implementation tests; no devices or RF."""
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);mode=p.add_mutually_exclusive_group();mode.add_argument('--gnss',action='store_true');mode.add_argument('--bluetooth-commit',action='store_true');mode.add_argument('--bluetooth-config',action='store_true');mode.add_argument('--neighbor-transactions',action='store_true');mode.add_argument('--interval-extended',action='store_true');mode.add_argument('--radio-settings',action='store_true');mode.add_argument('--signed-public-application',action='store_true');mode.add_argument('--module-config',action='store_true');mode.add_argument('--endpoint',action='store_true');mode.add_argument('--endpoint-lifetimes',action='store_true');mode.add_argument('--public-application',action='store_true');mode.add_argument('--traceroute',action='store_true');mode.add_argument('--traceroute-transit',action='store_true');mode.add_argument('--phone',action='store_true');mode.add_argument('--reboot',action='store_true');mode.add_argument('--nodeinfo',action='store_true');mode.add_argument('--settings-edit',action='store_true');mode.add_argument('--settings-commit',action='store_true');mode.add_argument('--device-config',action='store_true');mode.add_argument('--wall-clock',action='store_true');mode.add_argument('--normal-tx',action='store_true');mode.add_argument('--normal-use-timing',action='store_true');mode.add_argument('--normal-use-policy',action='store_true');mode.add_argument('--unchanged-settings',action='store_true');mode.add_argument('--fixed-position',action='store_true');mode.add_argument('--input-event',action='store_true');mode.add_argument('--carousel',action='store_true');mode.add_argument('--display-names',action='store_true');mode.add_argument('--settings-failure',action='store_true');mode.add_argument('--channel-mute',action='store_true');mode.add_argument('--primary-channel-helper',action='store_true');mode.add_argument('--primary-channel',action='store_true');mode.add_argument('--channel-privacy-transport',action='store_true');p.add_argument('--ble-diagnostics',action='store_true');p.add_argument('--short-enums',action='store_true')
mode.add_argument('--local-clock',action='store_true')
mode.add_argument('--display-reports', action='store_true', help='Test admitted RF telemetry display reports')
p.add_argument('--xiao-expansion-oled', action='store_true', help='Test display settings on an XIAO with the declared OLED')
a=p.parse_args()
if a.xiao_expansion_oled and not (a.display_names or a.local_clock):
    p.error('--xiao-expansion-oled requires --display-names')
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
app=Path(__file__).resolve().parents[1];vendor=app.parent/'zephcore/adapters/bench/vendor';mono=app.parent/'zephcore/lib/monocypher';proto=app.parent/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
host_link=[]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
if a.short_enums:flags+=['-fshort-enums']
if a.local_clock:flags+=['-DCONFIG_MT_LOCAL_CLOCK=1','-DCONFIG_MT_GENERIC_THREAD=1']
if a.display_reports:flags+=['-DCONFIG_MT_TELEMETRY_REPORT_FRAMES=1']
if a.xiao_expansion_oled:flags+=['-DCONFIG_MT_XIAO_EXPANSION_OLED=1','-DCONFIG_MT_DISPLAY=1']
if a.ble_diagnostics:flags+=['-DCONFIG_MT_BLE_DIAGNOSTICS=1']
if a.settings_failure:flags+=['-DEXPECT_REPAIRED=1']
if a.carousel or a.display_names:flags+=['-O2']
commands=[];objects=[]
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(src.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
test='gnss_companion_tests' if a.gnss else 'bluetooth_commit_tests' if a.bluetooth_commit else 'bluetooth_config_tests' if a.bluetooth_config else 'neighbor_transaction_cases' if a.neighbor_transactions else 'channel_privacy_transport_cases' if a.channel_privacy_transport else 'normal_use_policy_cases' if a.normal_use_policy else 'normal_use_timing_cases' if a.normal_use_timing else 'endpoint_lifetime_cases' if a.endpoint_lifetimes else 'interval_extended_tests' if a.interval_extended else 'radio_config_cases' if a.radio_settings else 'signed_public_application_cases' if a.signed_public_application else 'module_config_cases' if a.module_config else 'public_application_cases' if a.public_application else 'primary_channel_cases' if a.primary_channel else 'primary_channel_helper_cases' if a.primary_channel_helper else 'display_names_tests' if a.display_names else 'settings_failure_tests' if a.settings_failure else 'channel_mute_tests' if a.channel_mute else 'carousel_tests' if a.carousel else 'input_event_tests' if a.input_event else 'fixed_position_tests' if a.fixed_position else 'unchanged_settings_tests' if a.unchanged_settings else 'normal_tx_tests' if a.normal_tx else 'wall_clock_tests' if a.wall_clock else 'device_config_tests' if a.device_config else 'settings_commit_tests' if a.settings_commit else 'settings_edit_tests' if a.settings_edit else 'reboot_tests' if a.reboot else 'nodeinfo_tests' if a.nodeinfo else 'phone_companion_tests' if a.phone else 'traceroute_transit_tests' if a.traceroute_transit else 'traceroute_tests' if a.traceroute else 'endpoint_tests' if (a.endpoint or a.endpoint_lifetimes) else 'companion_tests'
if a.local_clock:test='local_clock_cases'
if a.display_reports:test='stock_report_frames/radio'
extra=[str(app/'src/pki/PkiSigner.cpp'),str(app/'src/pki/PkiIdentity.cpp')] if a.traceroute_transit else []
if a.local_clock:extra+=[str(app/'src/clock/LocalTime.cpp')]
cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/('tests/'+test+'.cpp')),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*extra,*objects,*host_link,'-o',str(out/'companion-tests')]
subprocess.run(cmd,check=True);commands.append(cmd)
r=subprocess.run([str(out/'companion-tests')],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'companion.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode();(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
sources=[Path(__file__).resolve(),app/('tests/'+test+'.cpp'),app/'tests/device_config_fixture.h',app/'src/Companion.h',app/'src/TraceRoute.h',app/'src/Config.h',app/'src/PrimaryChannel.hpp',app/'src/PublicApplication.h',app/'src/Endpoint.h',app/'src/Service.h',app/'src/pki/PrivateRuntime.h']
if a.neighbor_transactions:sources += [app/'src/NeighborQuery.h',app/'src/NeighborInfoPayload.h',app/'src/NeighborReplyWindow.h',app/'src/RadioDiagnostic.h',app/'tests/radio_diagnostic_cases.inc',vendor/'MtCodec.h',app/'tests/endpoint_fixture.h']
if a.endpoint_lifetimes or a.normal_use_timing or a.normal_use_policy or a.normal_use_policy:sources += [app/'src/RadioTiming.h',app/'src/PacketLifetime.h',vendor/'RadioPacketTiming.h',app/'src/TxAdmission.h',app/'tests/endpoint_fixture.h']
if a.interval_extended:sources += [app/'src/Discovery.h']
if a.radio_settings:sources += [app/'src/LoRaPresets.h',app/'src/USChannel.h',app/'src/RadioSettings.h',app/'src/SettingsJournal.h',app/'src/SettingsEdit.h']
if a.module_config:sources += [app/'src/ModuleSettings.h',app/'src/ModuleJournal.h',app/'src/ModuleEdit.h',app/'src/SettingsEdit.h',app/'src/SettingsCommit.h',app/'src/StatusMessagePayload.h',app/'src/PeerStatus.h',app/'src/NeighborInfoPayload.h',app/'src/TelemetryAnnouncement.h',app/'src/ModuleSchedule.h',app/'src/NeighborObservations.h',app/'src/ModuleController.h',app/'src/ModuleAnnouncement.h',app/'src/RangeTestPayload.h',app/'src/LocalStatsQuery.h']
if a.module_config:sources += [app/'src/PowerAnnouncement.h',app/'src/power/PowerReadings.h',app/'src/power/PowerMetricsAdapter.h',app/'src/ModuleIntervals.h',app/'src/NeighborIntervals.h',app/'src/OnlineObservations.h',app/'tests/module_controller_runtime_cases.inc']
if a.public_application or a.signed_public_application:sources += [app/'tests/endpoint_fixture.h']
if a.signed_public_application:sources += [app.parent/'zephcore/adapters/bench/vendor/MtCodec.h',app.parent/'zephcore/adapters/bench/vendor/MtSignature.h',app.parent/'zephcore/adapters/bench/vendor/MtSignature.cpp',app/'src/PeerStatus.h',app/'src/StatusMessagePayload.h']
if a.traceroute_transit:sources += [app/'src/Service.h',app/'src/pki/PkiSigner.cpp',app/'src/pki/PkiIdentity.cpp',vendor/'MtCodec.h',vendor/'RoutingCore.h']
if a.primary_channel or a.primary_channel_helper:sources += [vendor/'MtNodeInfo.h',vendor/'MtKeyCommand.h']
if a.channel_privacy_transport:sources += [app/'src/PositionPrecision.h',app/'src/PositionQuery.h',app/'src/ChannelRecord.h',app/'src/ChannelOrder.h']
if a.primary_channel:sources += [app/'src/Service.h',app/'src/Endpoint.h',app/'src/SettingsEdit.h',app/'src/SettingsJournal.h',app/'src/ui/Model.h',app/'src/pki/PrivateRuntime.h']
if a.gnss:sources += [app/'src/gnss/GnssSettings.h',app/'src/PositionQuery.h',app/'src/SettingsJournal.h',app/'src/SettingsEdit.h']
pass_lines=[line for line in r.stdout.splitlines() if line.startswith('PASS ')]
is_trace=a.traceroute or a.traceroute_transit
summary_prefixes=('PASS Neighbor public transactions complete', 'PASS channel privacy transport suite:', 'PASS endpoint suite:', 'PASS traceroute suite:', 'PASS traceroute transit suite:', 'PASS channel-mute suite:', 'PASS primary-channel suite:')
scenario_groups=sum(not line.startswith(summary_prefixes) for line in pass_lines)
summary={'status':'PASS','ASAN_UBSAN':True,'LeakSanitizer':'UNSUPPORTED_detect_leaks_0','short_enums':a.short_enums,'hardware':False,'random_serial_bytes':0 if a.gnss or a.bluetooth_commit or a.bluetooth_config or a.neighbor_transactions or a.interval_extended or a.radio_settings or a.signed_public_application or (a.endpoint or a.endpoint_lifetimes) or a.module_config or a.public_application or is_trace or a.phone or a.reboot or a.nodeinfo or a.settings_edit or a.settings_commit or a.device_config or a.wall_clock or a.normal_tx or a.normal_use_timing or a.normal_use_policy or a.unchanged_settings or a.fixed_position or a.input_event or a.carousel or a.display_names or a.channel_mute or a.settings_failure or a.primary_channel_helper or a.primary_channel or a.channel_privacy_transport else 100000,'ble_diagnostics':a.ble_diagnostics,'scenario_groups':scenario_groups if a.gnss or a.bluetooth_commit or a.bluetooth_config or a.neighbor_transactions or a.interval_extended or a.radio_settings or is_trace or a.phone or a.reboot or a.settings_edit or a.settings_commit or a.device_config or a.wall_clock or a.normal_tx or a.normal_use_timing or a.normal_use_policy or a.unchanged_settings or a.fixed_position or a.input_event or a.carousel or a.display_names or a.channel_mute or a.settings_failure or a.primary_channel_helper or a.primary_channel or a.channel_privacy_transport else 20 if a.nodeinfo else (26 if a.ble_diagnostics else 24) if a.phone else scenario_groups if a.signed_public_application or (a.endpoint or a.endpoint_lifetimes) or a.module_config or a.public_application else 24,'pass_summary_lines':sum(line.startswith(summary_prefixes) for line in pass_lines),'source_sha256':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}}
if a.local_clock:
 summary['scenario_groups']=scenario_groups;summary['random_streams']=0
if a.nodeinfo:summary['scenario_groups']=scenario_groups
if a.primary_channel_helper:
 summary['assertion_checks']=int(r.stdout.split('checks=')[1].split()[0])
 summary['scope']='Fixed LongFast key normalization and stored-record validation only; no channel caller integration or RF proof.'
(out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
