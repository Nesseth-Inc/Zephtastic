#!/usr/bin/env python3
"""Unchanged Python 2.7.11 position query API/callback through memory transports."""
import argparse,base64,struct,hashlib,importlib.metadata,inspect,json,os,subprocess,sys
from pathlib import Path
from unittest.mock import patch
from meshtastic import mesh_interface
from meshtastic.mesh_interface import MeshInterface
from meshtastic.protobuf import mesh_pb2,portnums_pb2
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-root',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);app=Path(__file__).resolve().parents[1]
assert importlib.metadata.version('meshtastic')=='2.7.11'
assert subprocess.check_output(['git','-C',str(a.stock_root),'rev-parse','HEAD'],text=True).strip()=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
cmd=[sys.executable,'-B',str(app/'scripts/test-position-query.py'),'--bridge','--output',str(out/'build')]
if a.short_enums:cmd+=['--short-enums']
subprocess.run(cmd,check=True);exe=out/'build/position-query-tests'
class Stock(MeshInterface):
 def __init__(self):
  super().__init__(timeout=1);self.currentPacketId=0x4d270000;self.myInfo=mesh_pb2.MyNodeInfo(my_node_num=0x12345678)
  self.localNode.nodeNum=0x12345678;self.localNode.localConfig.lora.hop_limit=3
  self.nodesByNum={0x12345678:{'num':0x12345678,'user':{'id':'!12345678'}},0x87654321:{'num':0x87654321,'user':{'id':'!87654321'}}}
  self.nodes={v['user']['id']:v for v in self.nodesByNum.values()};self.isConnected.set();self.sent=[]
 def _generatePacketId(self):
  with patch.object(mesh_interface.random,'randint',return_value=0x134400):return super()._generatePacketId()
 def _sendToRadio(self,message):self.sent.append(message.SerializeToString(deterministic=True))
cases=[];commands=[cmd];env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'}
for model in (81,127):
 for transport in ('usb','ble'):
  for mode in ('normal','ack-first','malformed','nak','zero','mqtt','nak-mqtt','wide-nak'):
   c=Stock()
   with patch.object(c,'waitForPosition',return_value=None):request=c.sendPosition(destinationId=0x87654321,wantResponse=True)
   raw=c.sent[-1];parsed=mesh_pb2.ToRadio.FromString(raw).packet
   assert parsed.decoded.portnum==3 and parsed.decoded.want_response and parsed.decoded.payload==b'' and not parsed.want_ack
   position=mesh_pb2.Position(latitude_i=123456789,longitude_i=-987654321,altitude=-35,time=1700000000,
    location_source=3,altitude_source=4,timestamp=1699999999,timestamp_millis_adjust=-123,altitude_hae=-10,
    altitude_geoidal_separation=-25,PDOP=135,HDOP=80,VDOP=109,gps_accuracy=3000,ground_speed=42,
    ground_track=12345,fix_quality=1,fix_type=3,sats_in_view=11,sensor_id=2,next_update=3600,seq_number=7,precision_bits=24)
   if mode=='zero':position.latitude_i=0;position.longitude_i=0;position.precision_bits=32
   payload=position.SerializeToString(deterministic=True)
   data=mesh_pb2.Data(portnum=3,payload=payload,request_id=parsed.id,bitfield=0)
   if mode in ('nak','nak-mqtt','wide-nak'):data=mesh_pb2.Data(portnum=5,payload=mesh_pb2.Routing(error_reason=256 if mode=='wide-nak' else mesh_pb2.Routing.NO_RESPONSE).SerializeToString(),request_id=parsed.id,bitfield=0)
   if mode in ('mqtt','nak-mqtt'):data.bitfield=1
   run=[str(exe),raw.hex(),data.SerializeToString(deterministic=True).hex(),str(model),transport,'normal' if mode in ('nak','zero','mqtt','nak-mqtt','wide-nak') else mode];commands.append(run)
   proc=subprocess.run(run,text=True,capture_output=True,env=env,check=True)
   records={'USB':[],'BLE':[],'RF':[]}
   for line in proc.stdout.splitlines():
    tag,value=line.split(' ',1);records[tag].append(bytes.fromhex(value))
   frame=records['RF'][0];iv=struct.pack('<IIII',parsed.id,0,0x12345678,0)
   decrypt=['openssl','enc','-d','-aes-128-ctr','-nosalt','-K',base64.b64decode('1PG7OiApB1nwvP+rz05pAQ==').hex(),'-iv',iv.hex()];commands.append(decrypt)
   plaintext=subprocess.run(decrypt,input=frame[16:],capture_output=True,check=True).stdout
   expected=mesh_pb2.Data(portnum=3,want_response=True,bitfield=2).SerializeToString(deterministic=True)
   assert plaintext==expected
   owner=records[transport.upper()];other=records['BLE' if transport=='usb' else 'USB']
   outputs=[mesh_pb2.FromRadio.FromString(v) for v in owner]
   locations=[v.packet for v in outputs if v.HasField('packet') and v.packet.decoded.portnum==3]
   assert not any(v.HasField('packet') and v.packet.decoded.request_id==parsed.id for v in map(mesh_pb2.FromRadio.FromString,other))
   exits=[]
   with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()),patch.object(mesh_interface.pub,'sendMessage',return_value=None),patch.object(mesh_interface,'our_exit',side_effect=lambda message:exits.append(message)):
    for value in owner:c._handleFromRadio(value)
   assert parsed.id not in c.responseHandlers
   if mode in ('nak','nak-mqtt','wide-nak'):
    assert not locations and not c._acknowledgment.receivedPosition and len(exits)==(0 if mode=='wide-nak' else 1)
   else:
    assert len(locations)==1 and locations[0].decoded.payload==payload and c._acknowledgment.receivedPosition
    assert c.nodesByNum[0x87654321]['position']['latitudeI']==position.latitude_i
    assert c.nodesByNum[0x87654321]['position']['longitudeI']==position.longitude_i
   cases.append({'model':model,'transport':transport,'mode':mode,'status':'PASS','request':raw.hex(),'rf_frame':records['RF'][0].hex(),'response_data':data.SerializeToString(deterministic=True).hex(),'owner_fromradio':[x.hex() for x in owner]})
# Two simultaneously pending unchanged stock callbacks, completed in reverse order.
for model in (81,127):
 for transport in ('usb','ble'):
  c=Stock()
  with patch.object(c,'waitForPosition',return_value=None):
   first=c.sendPosition(destinationId=0x87654321,wantResponse=True);second=c.sendPosition(destinationId=0x87654321,wantResponse=True)
  assert first.id!=second.id and first.id in c.responseHandlers and second.id in c.responseHandlers
  p1=mesh_pb2.Position(latitude_i=123456789,longitude_i=-987654321,precision_bits=24)
  p2=mesh_pb2.Position(latitude_i=-123456789,longitude_i=987654321,precision_bits=24)
  d1=mesh_pb2.Data(portnum=3,payload=p1.SerializeToString(deterministic=True),request_id=first.id,bitfield=0)
  d2=mesh_pb2.Data(portnum=3,payload=p2.SerializeToString(deterministic=True),request_id=second.id,bitfield=1)
  run=[str(exe),c.sent[0].hex(),d1.SerializeToString(deterministic=True).hex(),str(model),transport,'two-pending',c.sent[1].hex(),d2.SerializeToString(deterministic=True).hex()];commands.append(run)
  proc=subprocess.run(run,text=True,capture_output=True,env=env,check=True);records={'USB':[],'BLE':[],'RF':[]}
  for line in proc.stdout.splitlines():
   tag,value=line.split(' ',1);records[tag].append(bytes.fromhex(value))
  assert len(records['RF'])==2
  for packet,frame in zip((first,second),records['RF']):
   assert struct.unpack_from('<I',frame,8)[0]==packet.id
   decrypt=['openssl','enc','-d','-aes-128-ctr','-nosalt','-K',base64.b64decode('1PG7OiApB1nwvP+rz05pAQ==').hex(),'-iv',struct.pack('<IIII',packet.id,0,0x12345678,0).hex()];commands.append(decrypt)
   assert subprocess.run(decrypt,input=frame[16:],capture_output=True,check=True).stdout==mesh_pb2.Data(portnum=3,want_response=True,bitfield=2).SerializeToString(deterministic=True)
  owner=records[transport.upper()];other=records['BLE' if transport=='usb' else 'USB'];completed=[]
  assert not any(v.HasField('packet') and v.packet.decoded.request_id in (first.id,second.id) for v in map(mesh_pb2.FromRadio.FromString,other))
  with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()),patch.object(mesh_interface.pub,'sendMessage',return_value=None):
   for value in owner:
    message=mesh_pb2.FromRadio.FromString(value);is_position=message.HasField('packet') and message.packet.decoded.portnum==3
    c._acknowledgment.receivedPosition=False;c._handleFromRadio(value)
    if is_position:
     ident=message.packet.decoded.request_id;completed.append(ident)
     assert c._acknowledgment.receivedPosition and ident not in c.responseHandlers
     expected_id=second.id if len(completed)==1 else first.id;expected_pos=p2 if len(completed)==1 else p1
     assert ident==expected_id and message.packet.decoded.payload==expected_pos.SerializeToString(deterministic=True)
     assert c.nodesByNum[0x87654321]['position']['latitudeI']==expected_pos.latitude_i
     assert c.nodesByNum[0x87654321]['position']['longitudeI']==expected_pos.longitude_i
     if len(completed)==1:assert first.id in c.responseHandlers
  assert completed==[second.id,first.id] and not c.responseHandlers
  cases.append({'model':model,'transport':transport,'mode':'two-pending-out-of-order','status':'PASS','requests':[v.hex() for v in c.sent],'rf_frames':[v.hex() for v in records['RF']],'completed_ids':completed,'owner_fromradio':[v.hex() for v in owner]})
refs=[a.stock_root/'src/mesh/Router.cpp',a.stock_root/'src/modules/PositionModule.cpp',a.stock_root/'src/mesh/MeshModule.cpp',Path(inspect.getsourcefile(mesh_interface)).resolve(),app/'src/Companion.h',app/'src/Service.h',app/'src/Endpoint.h',app/'src/PositionQuery.h',app/'tests/position_query_bridge.cpp',Path(__file__).resolve()]
result={'status':'PASS','client_version':'2.7.11','stock_firmware_git_sha1':'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','short_enums':a.short_enums,'case_count':len(cases),'cases':cases,'hardware':False,'synthetic_stock_encoded_peer':True,'two_device_round_trip':False,'callback_unchanged':True,'physical_transport':False,'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in refs}}
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS actual stock position query: '+str(len(cases))+' cases')
