#!/usr/bin/env python3
"""Meshtastic 2.7.11 absent favorite API/parser interoperability; no hardware/RF."""
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
files=[app/'src/Companion.h',app/'src/NodeCache.h',app/'src/NodeJournal.h',app/'src/NodeAdmin.h',app/'tests/node_mute_fixture.h',app/'tests/favorite_noop_oracle.cpp',Path(__file__).resolve(),
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
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
oracle=out/'favorite-noop-oracle';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/favorite_noop_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(oracle)]
commands.append(cmd);subprocess.run(cmd,check=True)

check('favorite descriptor field',admin_pb2.AdminMessage.DESCRIPTOR.fields_by_name['set_favorite_node'].number==39)
check('unfavorite descriptor field',admin_pb2.AdminMessage.DESCRIPTOR.fields_by_name['remove_favorite_node'].number==40)
upstream=(stock/'src/modules/AdminModule.cpp').read_text()
favorite=upstream[upstream.index('    case meshtastic_AdminMessage_set_favorite_node_tag:'):upstream.index('    case meshtastic_AdminMessage_set_ignored_node_tag:')]
check('pinned stock handlers look up without creation',favorite.count('nodeDB->getMeshNode(')==2 and favorite.count('if (node != NULL)')==2 and 'getOrCreateMeshNode' not in favorite)
check('pinned stock default response is Routing.NONE','if (mp.decoded.want_response && !myReply)' in upstream and 'myReply = allocErrorResponse(meshtastic_Routing_Error_NONE, &mp);' in upstream)
report['upstream_scope']='Static inspection of exact pinned handlers/default response; no claim of executing full stock firmware.'
transcripts=[]
def records(state):return [mesh_pb2.FromRadio.FromString(bytes.fromhex(value)) for value in state['records_hex']]
def policy(state):return {key:state[key] for key in ('writes','generation','slot','count','dirty','favorite','missing_present','config_hex','durable','starts','sends','tx_armed')}
def errors(values,request):
 return [mesh_pb2.Routing.FromString(value.packet.decoded.payload).error_reason for value in values
         if value.HasField('packet') and value.packet.decoded.portnum==portnums_pb2.ROUTING_APP and value.packet.decoded.request_id==request.packet.id]
for model in (81,127):
 for transport in ('usb','ble'):
  label=f'{model}/{transport}';actual=client_fixture.StockClient(0x12345678,0x87654321);actual._localChannels=[]
  with (out/f'oracle-{model}-{transport}.stderr').open('w') as error_log:
   process=subprocess.Popen([str(oracle),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=error_log,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   def exchange(raw):
    process.stdin.write(transport+' '+raw.hex()+'\n');process.stdin.flush()
    check(label+' bounded adapter reply',bool(select.select([process.stdout],[],[],5)[0]));line=process.stdout.readline();check(label+' nonempty adapter reply',bool(line))
    row=json.loads(line);check(label+' no radio activation',row['starts']==0 and row['sends']==0 and not row['tx_armed'])
    transcripts.append({'model':model,'transport':transport,'request_hex':raw.hex(),'response':row});return row
   def call(set_favorite,target,expected=mesh_pb2.Routing.NONE):
    start=len(actual.sent)
    # Invoke the real public methods, including their real ensureSessionKey
    # preflight. Do not replace that method or invent an admin credential.
    if set_favorite:actual.localNode.setFavorite(target)
    else:actual.localNode.removeFavorite(target)
    pending=actual.sent[start:];check(label+' actual API emits one or preflight-plus-operation',len(pending) in (1,2))
    final_raw=None;final_request=None;final_row=None
    for index,raw in enumerate(pending):
     request=mesh_pb2.ToRadio.FromString(raw);packet=request.packet;admin=admin_pb2.AdminMessage.FromString(packet.decoded.payload)
     check(label+' actual local envelope',packet.id!=0 and packet.to==0x12345678 and packet.want_ack and packet.decoded.want_response and packet.pki_encrypted and packet.decoded.portnum==portnums_pb2.ADMIN_APP and packet.channel==0)
     last=index+1==len(pending)
     if not last:check(label+' actual session-config preflight',admin.WhichOneof('payload_variant')=='get_config_request' and admin.get_config_request==admin_pb2.AdminMessage.SESSIONKEY_CONFIG)
     else:
      field='set_favorite_node' if set_favorite else 'remove_favorite_node'
      expected_target=int(target[1:],16) if isinstance(target,str) else target
      check(label+' actual favorite API payload',admin.WhichOneof('payload_variant')==field and getattr(admin,field)==expected_target and not admin.session_passkey)
     row=exchange(raw);values=records(row)
     check(label+' correlated queue status',values[0].HasField('queueStatus') and values[0].queueStatus.mesh_packet_id==packet.id)
     replies=[v.packet for v in values if v.HasField('packet')]
     check(label+' exactly one correlated local response',len(replies)==1 and replies[0].decoded.request_id==packet.id and not replies[0].pki_encrypted and not replies[0].public_key)
     if last:
      check(label+' decoded expected Routing result',errors(values,request)==[expected]);final_raw=raw;final_request=request;final_row=row
     else:check(label+' preflight config response',replies[0].decoded.portnum==portnums_pb2.ADMIN_APP)
     # The actual favorite APIs install no local-node callback. Consume the
     # actual parser normally and inspect correlated Routing; do not add one.
     actual.accept(row['records_hex'])
    return final_raw,final_request,final_row
   try:
    hello=mesh_pb2.ToRadio(want_config_id=0x2600+model);state=exchange(hello.SerializeToString(deterministic=True));actual.accept(state['records_hex'])
    check(label+' initial seeded cache is dirty/nonempty',state['count']==1 and state['dirty'] and state['writes']==0)
    baseline=policy(state)
    for set_favorite in (True,False,True,False):
     raw,request,state=call(set_favorite,'!66667777');check(label+' missing request preserves exact policy/storage',policy(state)==baseline)
     check(label+' no invented NodeInfo',[v for v in records(state) if v.HasField('node_info')]==[] and 0x66667777 not in actual.nodesByNum)
     repeat=exchange(raw);check(label+' exact retry preserves policy and ACK',policy(repeat)==baseline and errors(records(repeat),request)==[mesh_pb2.Routing.NONE]);actual.accept(repeat['records_hex'])
    raw,request,state=call(True,0x87654321);check(label+' existing favorite persists',state['favorite'] and state['writes']==1 and not state['dirty'])
    check(label+' actual parser sees favorite',actual.nodesByNum[0x87654321].get('isFavorite') is True)
    clean=policy(state);raw,request,state=call(False,0x66667777);check(label+' clean durable state also preserved',policy(state)==clean)
    raw,request,state=call(False,0x87654321);check(label+' existing unfavorite persists',not state['favorite'] and state['writes']==2)
    updates=[v.node_info for v in records(state) if v.HasField('node_info')];check(label+' raw existing unfavorite update',len(updates)==1 and not updates[0].is_favorite)
    # Stock Python's incremental proto3-false merge can retain a stale true;
    # verify false through a fresh real parser/handshake, not a custom patch.
    fresh=client_fixture.StockClient(0x12345678,0x87654321);fresh._localChannels=[]
    state=exchange(mesh_pb2.ToRadio(want_config_id=0x2700+model).SerializeToString(deterministic=True));fresh.accept(state['records_hex'])
    check(label+' fresh actual parser sees unfavorite',not fresh.nodesByNum[0x87654321].get('isFavorite',False) and 0x66667777 not in fresh.nodesByNum)
    unchanged=policy(state)
    for target in (0x12345678,1,0xfffffffe):
     raw,request,state=call(True,target,mesh_pb2.Routing.BAD_REQUEST);check(label+' actual API invalid target nonmutation',policy(state)==unchanged)
   finally:
    process.stdin.close();process.wait(timeout=5)
   check(label+' adapter exit',process.returncode==0)
check('source stable',hashes()==before)
report.update(status='PASS',board_transport_cases=4,checks_count=len(report['checks']),source_sha256=before,
              native_adapter='production Companion/NodeAdmin/NodeJournal using memory USB/BLE framing',
              local_favorite_callback_claimed=False,physical_transport=False,
              stock_client_limitation='Incremental proto3 false values may leave stale dictionary state; fresh actual parser handshake verifies unfavorite.')
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(report['checks']),'checks; actual Meshtastic 2.7.11 setFavorite/removeFavorite, both models/transports; no physical interface')
