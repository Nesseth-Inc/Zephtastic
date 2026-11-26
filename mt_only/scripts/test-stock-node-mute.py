#!/usr/bin/env python3
"""Meshtastic 2.7.11 toggle-muted-node API/parser differential; no hardware/RF."""
import argparse,hashlib,importlib.metadata,importlib.util,inspect,json,os,select,shlex,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-root',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[2];app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';stock=a.stock_root.resolve()
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
client_fixture=module('m21_client',app/'tests/stock-private-client/client.py')
import meshtastic.node as node_module
from meshtastic.protobuf import admin_pb2,mesh_pb2,portnums_pb2
assert importlib.metadata.version('meshtastic')=='2.7.11'
node_path=Path(node_module.__file__).resolve();admin_path=Path(inspect.getfile(admin_pb2)).resolve();mesh_path=Path(inspect.getfile(mesh_pb2)).resolve()
files=[app/'src/Companion.h',app/'src/NodeCache.h',app/'src/NodeJournal.h',app/'tests/node_mute_fixture.h',app/'tests/node_mute_oracle.cpp',Path(__file__).resolve(),
       app/'tests/stock-private-client/client.py',stock/'src/modules/AdminModule.cpp',stock/'src/mesh/Channels.cpp',stock/'src/modules/TextMessageModule.cpp',
       stock/'src/graphics/draw/MessageRenderer.cpp',stock/'src/mesh/MeshService.cpp',stock/'src/mesh/TypeConversions.cpp',node_path,admin_path,mesh_path]
def hashes():return {str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
before=hashes();report={'status':'FAIL','hardware':False,'RF':False,'keys':False,'client_version':'2.7.11','short_enums':a.short_enums,'checks':[]}
def check(name,value):
 report['checks'].append({'name':name,'status':'PASS' if value else 'FAIL'})
 if not value:raise AssertionError(name)
expected={
 str(stock/'src/modules/AdminModule.cpp'):'ee758ed26686b718f592d7a9df048cefecfca13a2c7a5d01fd11192b0de73a28',
 str(stock/'src/mesh/Channels.cpp'):'beccffcbb50e42c5075697d5522303a75357dfa7d64b80197c301b313df881f0',
 str(stock/'src/modules/TextMessageModule.cpp'):'3660600a038958c059ac1069cf172742259b4660bcc1de26daac7399057d9719',
 str(stock/'src/graphics/draw/MessageRenderer.cpp'):'f96bf4b42744d3d40ac80b5c88bc03e10ad51a8608b942ed305774b6b6a340aa',
 str(stock/'src/mesh/MeshService.cpp'):'35559a376643fa21acddbd209ef2c4c20b4c0cc560a01cf3533f56f4dc7337bd',
 str(stock/'src/mesh/TypeConversions.cpp'):'12f110a1a3d9aeacf7e4d2320dede31ef8747399a0ce5c4d6ebe2d2d232e13a7',
 str(node_path):'74f0c5418458225258ca9455070ea3a559f6db09974170a0baf07fbf72b04136',
 str(admin_path):'7f385e9fed6a8c19bd95a268e10a1b9d0f1bf101ae453409d3a77a51d695d567',
 str(mesh_path):'87a4bd68876bdcd545a2a4ff8983e790c9b40becb940d611e0e588989370a8bf'}
for path,digest in expected.items():check('pinned '+Path(path).name,before[path]==digest)
check('pinned stock Git OID',subprocess.check_output(['git','-C',str(stock),'rev-parse','HEAD'],text=True).strip()=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c')
check('descriptor tag49',admin_pb2.AdminMessage.DESCRIPTOR.fields_by_name['toggle_muted_node'].number==49)
check('descriptor NodeInfo is_muted tag13',mesh_pb2.NodeInfo.DESCRIPTOR.fields_by_name['is_muted'].number==13)
check('canonical synthetic wire',admin_pb2.AdminMessage(toggle_muted_node=0x87654321).SerializeToString(deterministic=True).hex()=='8803a18695bb08')
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
oracle=out/'node-mute-oracle';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/node_mute_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(oracle)]
commands.append(cmd);subprocess.run(cmd,check=True)
def exchange(process,transport,raw):
 process.stdin.write(transport+' '+raw.hex()+'\n');process.stdin.flush();check(transport+' bounded response',bool(select.select([process.stdout],[],[],5)[0]));return json.loads(process.stdout.readline())
def records(state):return [mesh_pb2.FromRadio.FromString(bytes.fromhex(value)) for value in state['records_hex']]
def route_error(values,request):
 found=[]
 for value in values:
  if value.HasField('packet') and value.packet.decoded.portnum==portnums_pb2.ROUTING_APP and value.packet.decoded.request_id==request.packet.id:
   found.append(mesh_pb2.Routing.FromString(value.packet.decoded.payload).error_reason)
 return found
def make_admin(stock_client,target):
 message=admin_pb2.AdminMessage(toggle_muted_node=target);start=len(stock_client.sent);packet=stock_client.localNode._sendAdmin(message,wantResponse=True)
 check('actual Node._sendAdmin emitted one request',len(stock_client.sent)==start+1);raw=stock_client.sent[-1];request=mesh_pb2.ToRadio.FromString(raw)
 check('actual local admin envelope',request.packet.id==packet.id and request.packet.to==0x12345678 and request.packet.pki_encrypted and request.packet.want_ack and request.packet.decoded.want_response and request.packet.decoded.portnum==portnums_pb2.ADMIN_APP)
 decoded=admin_pb2.AdminMessage.FromString(request.packet.decoded.payload);check('actual tag49 payload',decoded.WhichOneof('payload_variant')=='toggle_muted_node' and decoded.toggle_muted_node==target)
 return raw,request
transcripts=[]
for model in (81,127):
 for transport in ('usb','ble'):
  stock_client=client_fixture.StockClient(0x12345678,0x87654321)
  stock_client._localChannels=[]
  with (out/f'oracle-{model}-{transport}.stderr').open('w') as errors:
   process=subprocess.Popen([str(oracle),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=errors,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   try:
    hello=mesh_pb2.ToRadio(want_config_id=0x2100+model);state=exchange(process,transport,hello.SerializeToString(deterministic=True));stock_client.accept(state['records_hex'])
    check(f'{model}/{transport} initial handshake unmuted',not stock_client.nodesByNum[0x87654321].get('isMuted',False))
    raw,request=make_admin(stock_client,0x87654321);state=exchange(process,transport,raw);decoded=records(state)
    check(f'{model}/{transport} first toggle success',state['muted'] and route_error(decoded,request)==[mesh_pb2.Routing.NONE])
    updates=[value.node_info for value in decoded if value.HasField('node_info') and value.node_info.num==0x87654321]
    check(f'{model}/{transport} update is_muted',len(updates)==1 and updates[0].is_muted)
    stock_client.accept(state['records_hex']);check(f'{model}/{transport} actual parser muted',stock_client.nodesByNum[0x87654321].get('isMuted') is True)
    writes=state['writes'];retry=exchange(process,transport,raw);check(f'{model}/{transport} exact retry inert',retry['muted'] and retry['writes']==writes)
    raw2,request2=make_admin(stock_client,0x87654321);state=exchange(process,transport,raw2);decoded=records(state)
    check(f'{model}/{transport} new request toggles',not state['muted'] and state['writes']==writes+1 and route_error(decoded,request2)==[mesh_pb2.Routing.NONE])
    unmute_updates=[value.node_info for value in decoded if value.HasField('node_info') and value.node_info.num==0x87654321]
    check(f'{model}/{transport} raw unmute readback truthful',len(unmute_updates)==1 and not unmute_updates[0].is_muted)
    # Python 2.7.11 MessageToDict omits proto3 false and its incremental
    # dictionary update consequently retains a prior true. Record that stock
    # client limitation; verify the supported full-handshake readback with a
    # fresh actual parser rather than pretending the stale key was cleared.
    stock_client.accept(state['records_hex']);check(f'{model}/{transport} known Python incremental false omission',stock_client.nodesByNum[0x87654321].get('isMuted') is True)
    fresh_client=client_fixture.StockClient(0x12345678,0x87654321);fresh_client._localChannels=[]
    hello2=mesh_pb2.ToRadio(want_config_id=0x3100+model);fresh=exchange(process,transport,hello2.SerializeToString(deterministic=True));fresh_client.accept(fresh['records_hex'])
    check(f'{model}/{transport} fresh actual parser unmuted handshake',not fresh_client.nodesByNum[0x87654321].get('isMuted',False))
    missing_raw,missing_request=make_admin(stock_client,0x76540000);before_writes=state['writes'];state=exchange(process,transport,missing_raw);decoded=records(state)
    check(f'{model}/{transport} missing explicit error',route_error(decoded,missing_request)==[mesh_pb2.Routing.BAD_REQUEST] and state['writes']==before_writes and not state['muted'])
    check(f'{model}/{transport} no RF',state['sends']==0);transcripts.append({'model':model,'transport':transport,'last_state':state})
   finally:
    process.stdin.close();process.wait(timeout=5)
   check(f'{model}/{transport} oracle exit',process.returncode==0)
check('source stable',hashes()==before)
report.update(status='PASS',board_transport_cases=4,checks_count=len(report['checks']),source_sha256=before,node_file=str(node_path),transcripts='transcripts.json',
              python_incremental_false_omission=True,
              stock_differences=['M21 returns correlated BAD_REQUEST for a missing peer where pinned stock silently does nothing',
                                 'Meshtastic 2.7.11 incremental proto3-false merge retains stale isMuted=true; raw M21 readback and a fresh handshake are false'])
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(report['checks']),'checks; actual Meshtastic 2.7.11 Node._sendAdmin/FromRadio parser, both boards/transports')
