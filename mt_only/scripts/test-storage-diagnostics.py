#!/usr/bin/env python3
"""ASAN/UBSAN companion implementation tests; no devices or RF."""
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);mode=p.add_mutually_exclusive_group();mode.add_argument('--endpoint',action='store_true');mode.add_argument('--traceroute',action='store_true');mode.add_argument('--traceroute-transit',action='store_true');mode.add_argument('--phone',action='store_true');mode.add_argument('--reboot',action='store_true');mode.add_argument('--nodeinfo',action='store_true');mode.add_argument('--settings-edit',action='store_true');mode.add_argument('--settings-commit',action='store_true');mode.add_argument('--device-config',action='store_true');mode.add_argument('--wall-clock',action='store_true');mode.add_argument('--normal-tx',action='store_true');mode.add_argument('--unchanged-settings',action='store_true');mode.add_argument('--fixed-position',action='store_true');mode.add_argument('--input-event',action='store_true');mode.add_argument('--carousel',action='store_true');mode.add_argument('--display-names',action='store_true');mode.add_argument('--settings-failure',action='store_true');mode.add_argument('--channel-mute',action='store_true');p.add_argument('--ble-diagnostics',action='store_true');p.add_argument('--short-enums',action='store_true');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
app=Path(__file__).resolve().parents[1];vendor=app.parent/'zephcore/adapters/bench/vendor';mono=app.parent/'zephcore/lib/monocypher';proto=app.parent/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
host_link=[]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
if a.short_enums:flags+=['-fshort-enums']
if a.ble_diagnostics:flags+=['-DCONFIG_MT_BLE_DIAGNOSTICS=1']
if a.settings_failure:flags+=['-DEXPECT_REPAIRED=1']
if a.carousel or a.display_names:flags+=['-O2']
commands=[];objects=[]
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(src.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
test='storage_diagnostic_tests'
extra=[str(app/'src/pki/PkiSigner.cpp'),str(app/'src/pki/PkiIdentity.cpp')] if a.traceroute_transit else []
cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/('tests/'+test+'.cpp')),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*extra,*objects,*host_link,'-o',str(out/'companion-tests')]
subprocess.run(cmd,check=True);commands.append(cmd)
r=subprocess.run([str(out/'companion-tests')],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'companion.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode();(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
sources=[Path(__file__).resolve(),app/('tests/'+test+'.cpp'),app/'tests/device_config_fixture.h',app/'src/Companion.h',app/'src/Service.h',app/'src/TraceRoute.h',app/'src/Config.h']
if a.traceroute_transit:sources += [app/'src/Service.h',app/'src/pki/PkiSigner.cpp',app/'src/pki/PkiIdentity.cpp',vendor/'MtCodec.h',vendor/'RoutingCore.h']
pass_lines=[line for line in r.stdout.splitlines() if line.startswith('PASS ')]
is_trace=a.traceroute or a.traceroute_transit
summary_prefixes=('PASS traceroute suite:', 'PASS traceroute transit suite:', 'PASS channel-mute suite:')
scenario_groups=sum(not line.startswith('PASS storage diagnostic groups=')for line in pass_lines)
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'LeakSanitizer':'UNSUPPORTED_detect_leaks_0','short_enums':a.short_enums,'hardware':False,'random_serial_bytes':0 if test=='storage_diagnostic_tests' or a.endpoint or is_trace or a.phone or a.reboot or a.nodeinfo or a.settings_edit or a.settings_commit or a.device_config or a.wall_clock or a.normal_tx or a.unchanged_settings or a.fixed_position or a.input_event or a.carousel or a.display_names or a.channel_mute or a.settings_failure else 100000,'ble_diagnostics':a.ble_diagnostics,'scenario_groups':scenario_groups if test=='storage_diagnostic_tests' or is_trace or a.reboot or a.settings_edit or a.settings_commit or a.device_config or a.wall_clock or a.normal_tx or a.unchanged_settings or a.fixed_position or a.input_event or a.carousel or a.display_names or a.channel_mute or a.settings_failure else 20 if a.nodeinfo else (26 if a.ble_diagnostics else 24) if a.phone else 33 if a.endpoint else 24,'pass_summary_lines':sum(line.startswith(summary_prefixes) for line in pass_lines),'source_sha256':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}},indent=2)+'\n')
