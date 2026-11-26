#!/usr/bin/env python3
"""Stock Python requests and decoder through the memory-only local-read bridge."""
import argparse,hashlib,importlib.metadata,inspect,json,os,subprocess
from pathlib import Path
from unittest.mock import patch
from meshtastic import mesh_interface
from meshtastic.mesh_interface import MeshInterface
from meshtastic.protobuf import mesh_pb2,portnums_pb2
p=argparse.ArgumentParser();p.add_argument('--executable',required=True);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(exist_ok=False)
assert importlib.metadata.version('meshtastic')=='2.7.11'
client=Path(inspect.getsourcefile(mesh_interface));assert hashlib.sha256(client.read_bytes()).hexdigest()=='8efd6483d8cc1c6a1acb1a6a75efb132a0a1e08a283fb523e83790270c60cc8e'
class Stock(MeshInterface):
 def __init__(self):
  super().__init__(timeout=1);self.currentPacketId=0x4d350000;self.myInfo=mesh_pb2.MyNodeInfo(my_node_num=0x12345678);self.localNode.nodeNum=0x12345678;self.localNode.localConfig.lora.hop_limit=3
  self.nodesByNum={0x12345678:{'num':0x12345678,'user':{'id':'!12345678'}}};self.nodes={'!12345678':self.nodesByNum[0x12345678]};self.isConnected.set();self.sent=[]
 def _sendToRadio(self,message):self.sent.append(message.SerializeToString(deterministic=True))
rows=[]
for model in (81,127):
 for transport in ('usb','ble'):
  for kind in ('nodeinfo','position'):
   c=Stock();callback=[]
   if kind=='position':
    with patch.object(c,'waitForPosition',return_value=None):c.sendPosition(destinationId=0x12345678,wantResponse=True)
   else:c.sendData(mesh_pb2.User(),destinationId=0x12345678,portNum=portnums_pb2.NODEINFO_APP,wantResponse=True,onResponse=lambda packet:callback.append(packet))
   wire=c.sent[-1];request=mesh_pb2.ToRadio.FromString(wire).packet
   run=subprocess.run([a.executable,str(model),transport,wire.hex()],capture_output=True,text=True,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   packets=[mesh_pb2.FromRadio.FromString(bytes.fromhex(line[4:])) for line in run.stdout.splitlines() if line.startswith('OUT ')]
   replies=[x.packet for x in packets if x.HasField('packet') and x.packet.decoded.portnum==request.decoded.portnum];assert len(replies)==1
   reply=replies[0];assert reply.decoded.request_id==request.id and getattr(reply,'from')==0x12345678 and reply.to==0x12345678 and not reply.want_ack
   with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()),patch.object(mesh_interface.pub,'sendMessage',return_value=None),patch.object(mesh_interface,'our_exit',return_value=None):
    for packet in packets:c._handleFromRadio(packet.SerializeToString())
   assert request.id not in c.responseHandlers
   if kind=='nodeinfo':assert len(callback)==1 and c.nodesByNum[0x12345678]['user']['id']=='!12345678'
   else:
    value=c.nodesByNum[0x12345678]['position'];assert value['latitudeI']==123456789 and value['longitudeI']==-456789012 and value['altitude']==-12
   rows.append({'model':model,'transport':transport,'kind':kind,'status':'PASS','request_hex':wire.hex(),'response_hex':reply.SerializeToString(deterministic=True).hex()})
(a.output/'results.json').write_text(json.dumps({'status':'PASS','stock_python':'2.7.11','client_sha256':hashlib.sha256(client.read_bytes()).hexdigest(),'hardware':False,'RF':False,'cases':rows},indent=2)+'\n');print('PASS 8 stock Python local-read round trips')
