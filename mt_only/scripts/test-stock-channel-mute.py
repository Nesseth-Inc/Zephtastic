#!/usr/bin/env python3
"""Meshtastic 2.7.11 primary channel-mute API/parser differential; no hardware/RF."""
import argparse,hashlib,importlib.metadata,importlib.util,inspect,json,os,select,shlex,shutil,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-root',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[2];app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';stock=a.stock_root.resolve()
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
client_fixture=module('m25_client',app/'tests/stock-private-client/client.py')
import meshtastic.node as node_module
from meshtastic.protobuf import admin_pb2,channel_pb2,mesh_pb2,portnums_pb2
assert importlib.metadata.version('meshtastic')=='2.7.11'
node_path=Path(node_module.__file__).resolve();admin_path=Path(inspect.getfile(admin_pb2)).resolve();channel_path=Path(inspect.getfile(channel_pb2)).resolve()
files=[app/'src/Companion.h',app/'src/Config.h',app/'src/SettingsEdit.h',app/'src/Service.h',app/'src/ui/Display.cpp',
       app/'tests/channel_mute_oracle.cpp',Path(__file__).resolve(),app/'tests/stock-private-client/client.py',
       stock/'src/mesh/Channels.h',stock/'src/mesh/Channels.cpp',stock/'src/modules/TextMessageModule.cpp',
       stock/'src/graphics/draw/MessageRenderer.cpp',stock/'src/modules/AdminModule.cpp',node_path,admin_path,channel_path]
def hashes():return {str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
before=hashes();report={'status':'FAIL','hardware':False,'RF':False,'keys':False,'client_version':'2.7.11','short_enums':a.short_enums,'checks':[]}
def check(name,value):
 report['checks'].append({'name':name,'status':'PASS' if value else 'FAIL'})
 if not value:raise AssertionError(name)
expected={str(stock/'src/mesh/Channels.h'):'8fa5aa4d5f1775210b09e1f08aa78b9da94fa2a9ae73b890b9d5fcda9dcc7f86',
 str(stock/'src/mesh/Channels.cpp'):'beccffcbb50e42c5075697d5522303a75357dfa7d64b80197c301b313df881f0',
 str(stock/'src/modules/TextMessageModule.cpp'):'3660600a038958c059ac1069cf172742259b4660bcc1de26daac7399057d9719',
 str(stock/'src/graphics/draw/MessageRenderer.cpp'):'f96bf4b42744d3d40ac80b5c88bc03e10ad51a8608b942ed305774b6b6a340aa',
 str(stock/'src/modules/AdminModule.cpp'):'ee758ed26686b718f592d7a9df048cefecfca13a2c7a5d01fd11192b0de73a28',
 str(node_path):'74f0c5418458225258ca9455070ea3a559f6db09974170a0baf07fbf72b04136'}
for path,digest in expected.items():check('pinned '+Path(path).name,before[path]==digest)
check('pinned stock Git OID',subprocess.check_output(['git','-C',str(stock),'rev-parse','HEAD'],text=True).strip()=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c')
check('descriptor Admin set_channel tag33',admin_pb2.AdminMessage.DESCRIPTOR.fields_by_name['set_channel'].number==33)
check('descriptor module_settings tag7',channel_pb2.ChannelSettings.DESCRIPTOR.fields_by_name['module_settings'].number==7)
check('descriptor is_muted tag2',channel_pb2.ModuleSettings.DESCRIPTOR.fields_by_name['is_muted'].number==2)
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(app/'tests'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
oracle=out/'channel-mute-oracle';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/channel_mute_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(oracle)]
commands.append(cmd);subprocess.run(cmd,check=True)
def exchange(process,transport,raw):
 process.stdin.write(transport+' '+raw.hex()+'\n');process.stdin.flush();check(transport+' bounded response',bool(select.select([process.stdout],[],[],5)[0]));return json.loads(process.stdout.readline())
def decoded(state):return [mesh_pb2.FromRadio.FromString(bytes.fromhex(value)) for value in state['records_hex']]
def route_error(records,request):
 values=[]
 for record in records:
  if record.HasField('packet') and record.packet.decoded.portnum==portnums_pb2.ROUTING_APP and record.packet.decoded.request_id==request.packet.id:values.append(mesh_pb2.Routing.FromString(record.packet.decoded.payload).error_reason)
 return values
def handshake(process,transport,client,nonce):
 client._localChannels=[]
 client.configId=nonce;hello=mesh_pb2.ToRadio(want_config_id=nonce);state=exchange(process,transport,hello.SerializeToString(deterministic=True));client.accept(state['records_hex'])
 check('actual parser populated eight channels',client.localNode.channels is not None and len(client.localNode.channels)==8);return state
def write_channel(client,muted,full=False):
 channel=client.localNode.getChannelByChannelIndex(0);check('actual getChannelByChannelIndex primary',channel is not None and channel.role==channel_pb2.Channel.PRIMARY)
 channel.settings.psk=(bytes.fromhex('d4f1bb3a20290759f0bcffabcf4e6901') if full else b'\x01')
 channel.settings.module_settings.is_muted=muted;start=len(client.sent);client.localNode.writeChannel(0)
 selected=[]
 for raw in client.sent[start:]:
  value=mesh_pb2.ToRadio.FromString(raw)
  if value.HasField('packet') and value.packet.decoded.portnum==portnums_pb2.ADMIN_APP:
   admin=admin_pb2.AdminMessage.FromString(value.packet.decoded.payload)
   if admin.WhichOneof('payload_variant')=='set_channel':selected.append((raw,value,admin))
 check('actual Node.writeChannel emitted one set_channel',len(selected)==1);raw,value,admin=selected[0]
 check('actual set_channel carries primary mute',admin.set_channel.index==0 and admin.set_channel.role==channel_pb2.Channel.PRIMARY and admin.set_channel.settings.module_settings.is_muted==muted)
 return raw,value
def get_channel(client):
 message=admin_pb2.AdminMessage(get_channel_request=1);start=len(client.sent);client.localNode._sendAdmin(message,wantResponse=True)
 selected=[]
 for raw in client.sent[start:]:
  value=mesh_pb2.ToRadio.FromString(raw)
  if value.HasField('packet') and value.packet.decoded.portnum==portnums_pb2.ADMIN_APP:selected.append((raw,value))
 check('actual Node._sendAdmin emitted one channel getter',len(selected)==1);return selected[0]
transcripts=[]
for model in (81,127):
 for transport in ('usb','ble'):
  client=client_fixture.StockClient(0x12345678,0x87654321)
  with (out/f'oracle-{model}-{transport}.stderr').open('w') as errors:
   process=subprocess.Popen([str(oracle),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=errors,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   try:
    initial=handshake(process,transport,client,0x25000000+model);check(f'{model}/{transport} initial unmuted',not client.localNode.channels[0].settings.module_settings.is_muted)
    raw,request=write_channel(client,True);state=exchange(process,transport,raw);records=decoded(state)
    check(f'{model}/{transport} mute persisted',state['muted'] and route_error(records,request)==[mesh_pb2.Routing.NONE] and state['receiving'] and state['armed'])
    raw_get,get_request=get_channel(client);state=exchange(process,transport,raw_get);records=decoded(state)
    replies=[admin_pb2.AdminMessage.FromString(r.packet.decoded.payload) for r in records if r.HasField('packet') and r.packet.decoded.portnum==portnums_pb2.ADMIN_APP and r.packet.decoded.request_id==get_request.packet.id]
    check(f'{model}/{transport} actual getter parser true',len(replies)==1 and replies[0].get_channel_response.settings.module_settings.is_muted)
    fresh=client_fixture.StockClient(0x12345678,0x87654321);state=handshake(process,transport,fresh,0x26000000+model)
    check(f'{model}/{transport} fresh handshake true',fresh.localNode.channels[0].settings.module_settings.is_muted)
    raw,request=write_channel(fresh,False,full=True);state=exchange(process,transport,raw);records=decoded(state)
    check(f'{model}/{transport} full-key unmute persisted',not state['muted'] and route_error(records,request)==[mesh_pb2.Routing.NONE] and state['receiving'] and state['armed'])
    final=client_fixture.StockClient(0x12345678,0x87654321);state=handshake(process,transport,final,0x27000000+model)
    check(f'{model}/{transport} fresh handshake false',not final.localNode.channels[0].settings.module_settings.is_muted)
    check(f'{model}/{transport} no RF',state['sends']==0);transcripts.append({'model':model,'transport':transport,'initial':initial,'final':state})
   finally:
    process.stdin.close();process.wait(timeout=5)
   check(f'{model}/{transport} oracle exit',process.returncode==0)
check('stock Channels direct-vs-broadcast policy','isMutedForPacket' in (stock/'src/mesh/Channels.cpp').read_text() and 'is_muted' in (stock/'src/mesh/Channels.cpp').read_text())
check('source stable',hashes()==before)
report.update(status='PASS',board_transport_cases=4,checks_count=len(report['checks']),source_sha256=before,transcripts='transcripts.json',
              stock_differences=['Only primary default-Public channel mute is writable; secondary/custom/AEAD channel settings remain refused',
                                 'External-notification alert_bell override is absent because that module is excluded'])
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(report['checks']),'checks; actual Meshtastic 2.7.11 Node.writeChannel/_sendAdmin/FromRadio, both boards/transports')
