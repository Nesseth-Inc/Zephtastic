#!/usr/bin/env python3
"""M28 telemetry query native tests; memory-only, no hardware or RF."""
import argparse,hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');p.add_argument('--responder',action='store_true');p.add_argument('--environment',action='store_true');p.add_argument('--power',action='store_true');p.add_argument('--bridge',action='store_true');p.add_argument('--local',action='store_true');p.add_argument('--local-reads',action='store_true');p.add_argument('--local-stats',action='store_true');p.add_argument('--public-local-stats',action='store_true');p.add_argument('--remote-variants',action='store_true');a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
vendor=app.parent/'zephcore/adapters/bench/vendor';mono=app.parent/'zephcore/lib/monocypher';proto=app.parent/'zephcore/bench/upstream-differential/source'
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
test_source=app/('tests/power_transport_tests.cpp' if a.power else 'tests/environment_transport_tests.cpp' if a.environment else 'tests/remote_telemetry_query_tests.cpp' if a.remote_variants else 'tests/public_local_stats_query_tests.cpp' if a.public_local_stats else 'tests/local_stats_query_tests.cpp' if a.local_stats else 'tests/local_reads_tests.cpp' if a.local_reads else 'tests/local_telemetry_tests.cpp' if a.local else 'tests/telemetry_responder_tests.cpp' if a.responder else 'tests/telemetry_query_bridge.cpp' if a.bridge else 'tests/telemetry_query_tests.cpp')
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
exe=out/'telemetry-query-tests';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(test_source),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects]
if sys.platform.startswith('linux'):cmd+=['-lcrypto']
if a.local_stats or a.public_local_stats or a.remote_variants:cmd+=[str(app/'src/pki/PkiIdentity.cpp')]
cmd+=['-o',str(exe)];commands.append(cmd);subprocess.run(cmd,check=True)
if a.bridge:
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');raise SystemExit(0)
run=subprocess.run([str(exe)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr);run.check_returncode()
sources=[app/'src/PowerQuery.h',app/'src/power/PowerReadings.h',app/'src/power/PowerMetricsAdapter.h',app/'src/EnvironmentQuery.h',app/'src/environment/EnvironmentMetricsAdapter.h',app/'src/Companion.h',app/'src/Service.h',app/'src/Endpoint.h',app/'src/TelemetryQuery.h',vendor/'MtCodec.h',app/'tests/endpoint_fixture.h',app/'tests/telemetry_query_fixture.h',test_source,Path(__file__).resolve()]
if a.local_stats or a.public_local_stats or a.remote_variants:sources += [app/'src/LocalStatsQuery.h',app/'src/RemoteTelemetryQuery.h',app/'src/pki/PrivateApplication.h',app/'src/pki/PkiIdentity.cpp']
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'optimized':True,'short_enums':a.short_enums,'hardware':False,'RF':False,'scenario_groups':sum(line.startswith('PASS ') for line in run.stdout.splitlines()),'board_transport_matrix_cases':0 if a.responder or a.local_stats or a.public_local_stats else 4,'responder_board_models':2 if a.responder else 0,'source_sha256':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}},indent=2)+'\n')
