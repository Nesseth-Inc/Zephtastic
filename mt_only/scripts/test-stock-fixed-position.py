#!/usr/bin/env python3
"""Meshtastic 2.7.11 fixed-position API/parser differential; no hardware/RF."""
import argparse,hashlib,importlib.metadata,importlib.util,inspect,json,os,select,shlex,shutil,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[2];app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
client=module('m18_client',app/'tests/stock-private-client/client.py')
from meshtastic.node import Node
from meshtastic.protobuf import admin_pb2,config_pb2,mesh_pb2,portnums_pb2
assert importlib.metadata.version('meshtastic')=='2.7.11'
files=[app/'src/Config.h',app/'src/SettingsJournal.h',app/'src/SettingsEdit.h',app/'src/Companion.h',
       app/'tests/fixed_position_oracle.cpp',Path(__file__).resolve(),app/'tests/stock-private-client/client.py',
       proto/'admin.pb.h',proto/'config.pb.h',proto/'mesh.pb.h']
def hashes():return {str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
before=hashes();report={'status':'FAIL','hardware':False,'RF':False,'GNSS':False,'keys':False,'client_version':'2.7.11','short_enums':a.short_enums,'checks':[]}
def check(name,value):
 report['checks'].append({'name':name,'status':'PASS' if value else 'FAIL'})
 if not value:raise AssertionError(name)
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
oracle=out/'fixed-position-oracle';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/fixed_position_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(oracle)]
commands.append(cmd);subprocess.run(cmd,check=True)

def admin_request(stock,action,expected):
 start=len(stock.sent);action();selected=[]
 for raw in stock.sent[start:]:
  request=mesh_pb2.ToRadio.FromString(raw)
  if not request.HasField('packet') or request.packet.decoded.portnum!=portnums_pb2.ADMIN_APP:continue
  admin=admin_pb2.AdminMessage.FromString(request.packet.decoded.payload)
  if admin.WhichOneof('payload_variant')==expected:selected.append((raw,request,admin))
 check('actual Node API emitted one '+expected,len(selected)==1);return selected[0]
def exchange(process,transport,raw):
 process.stdin.write(transport+' '+raw.hex()+'\n');process.stdin.flush();check(transport+' bounded response',bool(select.select([process.stdout],[],[],5)[0]));return json.loads(process.stdout.readline())
def routes(records,request):
 result=[]
 for record in records:
  if record.HasField('packet') and record.packet.decoded.portnum==portnums_pb2.ROUTING_APP and record.packet.decoded.request_id==request.packet.id:
   result.append(mesh_pb2.Routing.FromString(record.packet.decoded.payload))
 return result
def position_node(records,local):
 values=[r.node_info for r in records if r.HasField('node_info') and r.node_info.num==local]
 return values[-1] if values else None
def handshake_request(nonce):return mesh_pb2.ToRadio(want_config_id=nonce).SerializeToString(deterministic=True)

transcripts=[]
for model in (81,127):
 for transport in ('usb','ble'):
  local=0x12345678;stock=client.StockClient(local,0x87654321)
  with (out/f'oracle-{model}-{transport}.stderr').open('w') as errors:
   process=subprocess.Popen([str(oracle),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=errors,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   try:
    raw,request,admin=admin_request(stock,lambda:stock.localNode.setFixedPosition(44.9876543,-93.1234567,287),'set_fixed_position')
    state=exchange(process,transport,raw);records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']]
    check(f'{model}/{transport} set correlated success',len(routes(records,request))==1 and routes(records,request)[0].error_reason==mesh_pb2.Routing.NONE)
    node=position_node(records,local);check(f'{model}/{transport} set node update',node is not None and node.HasField('position') and node.position.latitude_i==449876543 and node.position.longitude_i==-931234567 and node.position.altitude==287)
    stock.accept(state['records_hex']);parsed=stock.nodesByNum[local]['position']
    check(f'{model}/{transport} real parser usable position',parsed['latitudeI']==449876543 and parsed['longitudeI']==-931234567 and parsed['altitude']==287 and abs(parsed['latitude']-44.9876543)<1e-9 and abs(parsed['longitude']+93.1234567)<1e-9)
    check(f'{model}/{transport} no RF on set',state['sends']==0 and state['fixed'] and state['writes']==1)

    other='ble' if transport=='usb' else 'usb';get=admin_pb2.AdminMessage(get_config_request=admin_pb2.AdminMessage.POSITION_CONFIG)
    packet=mesh_pb2.MeshPacket(to=local,id=0x18180001,want_ack=True);packet.decoded.portnum=portnums_pb2.ADMIN_APP;packet.decoded.want_response=True;packet.decoded.payload=get.SerializeToString(deterministic=True)
    state=exchange(process,other,mesh_pb2.ToRadio(packet=packet).SerializeToString(deterministic=True));records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']]
    replies=[]
    for record in records:
     if record.HasField('packet') and record.packet.decoded.portnum==portnums_pb2.ADMIN_APP:replies.append(admin_pb2.AdminMessage.FromString(record.packet.decoded.payload))
    check(f'{model}/{transport} cross-client PositionConfig readback',len(replies)==1 and replies[0].get_config_response.position.fixed_position and replies[0].get_config_response.position.gps_mode==config_pb2.Config.PositionConfig.NOT_PRESENT)

    raw,request,admin=admin_request(stock,lambda:stock.localNode.removeFixedPosition(),'remove_fixed_position')
    state=exchange(process,transport,raw);records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']]
    check(f'{model}/{transport} remove correlated success',len(routes(records,request))==1 and routes(records,request)[0].error_reason==mesh_pb2.Routing.NONE)
    tombstone=position_node(records,local);check(f'{model}/{transport} removal tombstone empty',tombstone is not None and tombstone.HasField('position') and not tombstone.position.ListFields())
    stock.accept(state['records_hex']);check(f'{model}/{transport} real parser clears stale position',not stock.nodesByNum[local].get('position'))
    check(f'{model}/{transport} no RF on remove',state['sends']==0 and not state['fixed'] and state['writes']==2)

    process.stdin.write('restart\n');process.stdin.flush();restarted=json.loads(process.stdout.readline());check(f'{model}/{transport} simulated restart removed',restarted['restart'] and not restarted['fixed'])
    fresh=client.StockClient(local,0x87654321);state=exchange(process,transport,handshake_request(0x1818))
    handshake_records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']]
    relevant=[v for v,r in zip(state['records_hex'],handshake_records) if r.HasField('node_info') or (r.HasField('config') and r.config.HasField('position'))]
    fresh.accept(relevant)
    check(f'{model}/{transport} reconnect handshake has position config and local node',
          any(r.HasField('config') and r.config.HasField('position') for r in handshake_records) and any(r.HasField('node_info') and r.node_info.num==local for r in handshake_records))
    check(f'{model}/{transport} reconnect omits removed position',not fresh.nodesByNum[local].get('position'))

    raw,request,admin=admin_request(fresh,lambda:fresh.localNode.setFixedPosition(0,0,0),'set_fixed_position')
    check(f'{model}/{transport} stock zero helper emits empty submessage',not admin.set_fixed_position.ListFields())
    state=exchange(process,transport,raw);records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']];fresh.accept(state['records_hex'])
    node=position_node(records,local);check(f'{model}/{transport} empty request reads explicit zero coordinates',node is not None and node.position.HasField('latitude_i') and node.position.HasField('longitude_i') and node.position.latitude_i==0 and node.position.longitude_i==0 and not node.position.HasField('altitude'))
    parsed=fresh.nodesByNum[local]['position'];check(f'{model}/{transport} parser retains equator meridian zeros','latitudeI' in parsed and 'longitudeI' in parsed and parsed['latitude']==0.0 and parsed['longitude']==0.0)
    check(f'{model}/{transport} zero request no RF',state['sends']==0 and state['fixed'])

    fresh.localNode.localConfig.position.CopyFrom(config_pb2.Config.PositionConfig(fixed_position=True,gps_mode=config_pb2.Config.PositionConfig.NOT_PRESENT))
    raw,request,admin=admin_request(fresh,lambda:fresh.localNode.writeConfig('position'),'set_config')
    state=exchange(process,other,raw);records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']]
    check(f'{model}/{transport} real writeConfig position echo accepted',len(routes(records,request))==1 and routes(records,request)[0].error_reason==mesh_pb2.Routing.NONE and state['sends']==0)
    transcripts.append({'model':model,'transport':transport,'set_admin_hex':admin.SerializeToString(deterministic=True).hex(),'final_state':state})
   finally:
    process.stdin.close();process.wait(timeout=5)
   check(f'{model}/{transport} oracle exit',process.returncode==0)
check('stock Node implementation hash recorded',bool(hashlib.sha256(inspect.getsource(Node).encode()).hexdigest()))
check('source stable',hashes()==before);report.update(status='PASS',source_sha256=before,node_api_sha256=hashlib.sha256(inspect.getsource(Node).encode()).hexdigest(),board_transport_cases=4)
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(report['checks']),'checks; actual Meshtastic 2.7.11 fixed-position APIs and FromRadio parser, memory-only USB/BLE')
