#!/usr/bin/env python3
"""Pinned Python DeviceMetrics API and callback through memory-only transports."""
import argparse, base64, hashlib, importlib.metadata, inspect, json, os, struct, subprocess, sys
from pathlib import Path
from unittest.mock import patch
from meshtastic import mesh_interface
from meshtastic.mesh_interface import MeshInterface
from meshtastic.protobuf import mesh_pb2, telemetry_pb2

p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-root',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);app=Path(__file__).resolve().parents[1]
assert importlib.metadata.version('meshtastic')=='2.7.11'
client_source=Path(inspect.getsourcefile(mesh_interface)).resolve()
stock_source=a.stock_root/'src/modules/Telemetry/DeviceTelemetry.cpp'
assert hashlib.sha256(client_source.read_bytes()).hexdigest()=='8efd6483d8cc1c6a1acb1a6a75efb132a0a1e08a283fb523e83790270c60cc8e'
assert hashlib.sha256(stock_source.read_bytes()).hexdigest()=='481f3a5be01cf60a8eeb420ec67d710f7cb8daf6086b1ce8b96920ebcc7fea67'
cmd=[sys.executable,'-B',str(app/'scripts/test-telemetry-query.py'),'--bridge','--output',str(out/'build')]
if a.short_enums:cmd+=['--short-enums']
subprocess.run(cmd,check=True);exe=out/'build/telemetry-query-tests'

class Stock(MeshInterface):
 def __init__(self,warm=False):
  super().__init__(timeout=1);self.currentPacketId=0x4d280000;self.myInfo=mesh_pb2.MyNodeInfo(my_node_num=0x12345678)
  self.localNode.nodeNum=0x12345678;self.localNode.localConfig.lora.hop_limit=3
  self.nodesByNum={0x12345678:{'num':0x12345678,'user':{'id':'!12345678'}},0x87654321:{'num':0x87654321,'user':{'id':'!87654321'}}}
  if warm:self.nodesByNum[0x12345678]['deviceMetrics']={'batteryLevel':73,'voltage':3.8,'channelUtilization':3.25,'airUtilTx':1.5,'uptimeSeconds':999}
  self.nodes={v['user']['id']:v for v in self.nodesByNum.values()};self.isConnected.set();self.sent=[]
 def _generatePacketId(self):
  with patch.object(mesh_interface.random,'randint',return_value=0x134400):return super()._generatePacketId()
 def _sendToRadio(self,message):self.sent.append(message.SerializeToString(deterministic=True))

env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'};cases=[];commands=[cmd]
def submit(c):
 with patch.object(c,'waitForTelemetry',return_value=None):c.sendTelemetry(destinationId=0x87654321,wantResponse=True)
 return mesh_pb2.ToRadio.FromString(c.sent[-1]).packet
def telemetry(sparse=False,powered=101,uptime=123456):
 t=telemetry_pb2.Telemetry(time=1700000000)
 t.device_metrics.uptime_seconds=uptime
 if not sparse:
  t.device_metrics.battery_level=powered;t.device_metrics.voltage=4.2;t.device_metrics.channel_utilization=12.5;t.device_metrics.air_util_tx=0.25
 return t
def execute(args):
 run=[str(exe),*args];commands.append(run);proc=subprocess.run(run,text=True,capture_output=True,env=env,check=True)
 records={'USB':[],'BLE':[],'RF':[]}
 for line in proc.stdout.splitlines():
  tag,value=line.split(' ',1);records[tag].append(bytes.fromhex(value))
 return records
def query_wire(packet,frame):
 decrypt=['openssl','enc','-d','-aes-128-ctr','-nosalt','-K',base64.b64decode('1PG7OiApB1nwvP+rz05pAQ==').hex(),'-iv',struct.pack('<IIII',packet.id,0,0x12345678,0).hex()]
 commands.append(decrypt);plain=subprocess.run(decrypt,input=frame[16:],capture_output=True,check=True).stdout
 assert plain==mesh_pb2.Data(portnum=67,payload=b'\x12\x00',want_response=True,bitfield=2).SerializeToString(deterministic=True)
def deliver(c,records,transport):
 with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()),patch.object(mesh_interface.pub,'sendMessage',return_value=None),patch.object(mesh_interface,'our_exit',return_value=None):
  for value in records[transport.upper()]:c._handleFromRadio(value)

for model in (81,127):
 for transport in ('usb','ble'):
  for mode in ('cold','warm','ack-first','malformed','nak','sparse','powered','mqtt'):
   c=Stock(mode=='warm');packet=submit(c);raw=c.sent[-1]
   assert packet.decoded.portnum==67 and packet.decoded.want_response and not packet.want_ack
   if mode=='warm':assert telemetry_pb2.Telemetry.FromString(packet.decoded.payload).device_metrics.voltage>0
   else:assert packet.decoded.payload==b''
   response=telemetry(mode=='sparse',102 if mode=='powered' else 101,0 if mode=='sparse' else 123456)
   payload=response.SerializeToString(deterministic=True)
   data=mesh_pb2.Data(portnum=67,payload=payload,request_id=packet.id,bitfield=1 if mode=='mqtt' else 0)
   if mode=='nak':data=mesh_pb2.Data(portnum=5,payload=mesh_pb2.Routing(error_reason=mesh_pb2.Routing.NO_RESPONSE).SerializeToString(),request_id=packet.id,bitfield=0)
   records=execute([raw.hex(),data.SerializeToString(deterministic=True).hex(),str(model),transport,mode if mode in ('ack-first','malformed') else 'normal'])
   assert len(records['RF'])==1;query_wire(packet,records['RF'][0])
   other='USB' if transport=='ble' else 'BLE'
   assert not any(v.HasField('packet') and v.packet.decoded.request_id==packet.id for v in map(mesh_pb2.FromRadio.FromString,records[other]))
   outputs=[v.packet for v in map(mesh_pb2.FromRadio.FromString,records[transport.upper()]) if v.HasField('packet') and v.packet.decoded.portnum==67]
   deliver(c,records,transport);assert packet.id not in c.responseHandlers
   if mode=='nak':assert not outputs and not c._acknowledgment.receivedTelemetry and 'deviceMetrics' not in c.nodesByNum[0x87654321]
   else:
    assert len(outputs)==1 and outputs[0].decoded.payload==payload and c._acknowledgment.receivedTelemetry
    metrics=c.nodesByNum[0x87654321]['deviceMetrics'];assert metrics['uptimeSeconds']==response.device_metrics.uptime_seconds
    parsed=telemetry_pb2.Telemetry.FromString(outputs[0].decoded.payload)
    assert parsed.device_metrics.HasField('battery_level')==(mode!='sparse')
    assert parsed.device_metrics.HasField('voltage')==(mode!='sparse')
   cases.append({'model':model,'transport':transport,'mode':mode,'status':'PASS','request':raw.hex(),'rf_frame':records['RF'][0].hex(),'response':data.SerializeToString(deterministic=True).hex()})
  c=Stock();first=submit(c);second=submit(c);assert first.id!=second.id
  d1=mesh_pb2.Data(portnum=67,payload=telemetry(uptime=1).SerializeToString(deterministic=True),request_id=first.id,bitfield=0)
  d2=mesh_pb2.Data(portnum=67,payload=telemetry(uptime=2).SerializeToString(deterministic=True),request_id=second.id,bitfield=1)
  records=execute([c.sent[0].hex(),d1.SerializeToString(deterministic=True).hex(),str(model),transport,'two-pending',c.sent[1].hex(),d2.SerializeToString(deterministic=True).hex()])
  assert len(records['RF'])==2
  for packet,frame in zip((first,second),records['RF']):query_wire(packet,frame)
  completed=[]
  for value in records[transport.upper()]:
   message=mesh_pb2.FromRadio.FromString(value)
   if message.HasField('packet') and message.packet.decoded.portnum==67:completed.append(message.packet.decoded.request_id)
  deliver(c,records,transport);assert completed==[second.id,first.id] and not c.responseHandlers
  assert c.nodesByNum[0x87654321]['deviceMetrics']['uptimeSeconds']==1
  cases.append({'model':model,'transport':transport,'mode':'two-pending-out-of-order','status':'PASS','completed':completed})
refs=[client_source,stock_source,app/'src/TelemetryQuery.h',app/'src/Companion.h',app/'src/Service.h',app/'src/Endpoint.h',app.parent/'zephcore/adapters/bench/vendor/MtCodec.h',app/'tests/endpoint_fixture.h',app/'tests/telemetry_query_fixture.h',app/'tests/telemetry_query_bridge.cpp',Path(__file__).resolve()]
result={'status':'PASS','client_version':'2.7.11','stock_reference_revision':'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','stock_source_verified_by_sha256':True,'short_enums':a.short_enums,'case_count':len(cases),'cases':cases,'hardware':False,'physical_transport':False,'RF':False,'synthetic_peer_data':True,'client_api_and_callback_unchanged':True,'wait_and_pubsub_scheduling_intercepted':True,'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in refs}}
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS actual stock telemetry query: '+str(len(cases))+' cases')
