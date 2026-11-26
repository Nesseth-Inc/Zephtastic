#!/usr/bin/env python3
"""Stock Meshtastic Python client against the host simulator; no device access."""
import argparse,faulthandler,json,logging,os,select,subprocess,time
from pathlib import Path
from meshtastic.stream_interface import StreamInterface
from meshtastic.protobuf import mesh_pb2,admin_pb2,channel_pb2,config_pb2,portnums_pb2

p=argparse.ArgumentParser();p.add_argument('--sim',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
p.add_argument('--expected-version',default='2.8.1.ZT-m28-rx');a=p.parse_args()
logging.disable(logging.CRITICAL);faulthandler.dump_traceback_later(60,exit=True)
messages=[]
class Sim(StreamInterface):
 def __init__(self):
  self.proc=subprocess.Popen([str(a.sim.resolve()),'--stream'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  super().__init__(connectNow=False,timeout=10);self.connect();self.waitForConfig()
 def _writeBytes(self,b):self.proc.stdin.write(b);self.proc.stdin.flush()
 def _readBytes(self,n):
  ready,_,_=select.select([self.proc.stdout],[],[],.1)
  return os.read(self.proc.stdout.fileno(),n) if ready else b''
 def _handleFromRadio(self,b):
  r=mesh_pb2.FromRadio();r.ParseFromString(b);messages.append(r)
  return super()._handleFromRadio(b)
 def close(self):
  super().close();self.proc.stdin.close();self.proc.wait(timeout=3)
  assert self.proc.returncode==0,self.proc.stderr.read().decode()
def wait(predicate):
 end=time.monotonic()+8
 while time.monotonic()<end:
  if predicate():return
  time.sleep(.02)
 raise AssertionError('Expected simulator response missing')
def ack(ident,reason):
 def found():
  for r in messages:
   if r.HasField('packet') and r.packet.decoded.portnum==portnums_pb2.ROUTING_APP and r.packet.decoded.request_id==ident:
    route=mesh_pb2.Routing();route.ParseFromString(r.packet.decoded.payload)
    assert route.error_reason==reason
    return True
  return False
 wait(found)
iface=None;report={'hardware':False,'RF':False,'status':'FAIL'}
try:
 iface=Sim();assert iface.metadata.firmware_version==a.expected_version
 assert not iface.metadata.hasBluetooth and not iface.metadata.hasWifi and not iface.metadata.hasPKC
 assert not iface.localNode.localConfig.lora.tx_enabled
 sent=iface.localNode.setOwner(long_name='ZT Demo',short_name='DEMO');ack(sent.id,mesh_pb2.Routing.NONE)
 ch=iface.localNode.channels[0];ch.role=channel_pb2.Channel.PRIMARY;ch.settings.psk=b'\x01'
 msg=admin_pb2.AdminMessage();msg.set_channel.CopyFrom(ch)
 sent=iface.localNode._sendAdmin(msg,wantResponse=True);ack(sent.id,mesh_pb2.Routing.NONE)
 lora=iface.localNode.localConfig.lora;lora.region=config_pb2.Config.LoRaConfig.US
 msg=admin_pb2.AdminMessage();msg.set_config.lora.CopyFrom(lora)
 sent=iface.localNode._sendAdmin(msg,wantResponse=True);ack(sent.id,mesh_pb2.Routing.NONE)
 sent=iface.sendText('Stock client local endpoint',destinationId=f'!{iface.myInfo.my_node_num:08x}',wantAck=True)
 ack(sent.id,mesh_pb2.Routing.NONE)
 wait(lambda:any(r.HasField('packet') and r.packet.id==sent.id and r.packet.decoded.portnum==1 and r.packet.decoded.payload==b'Stock client local endpoint' for r in messages))
 sent=iface.sendText('Disabled radio assertion',wantAck=True);ack(sent.id,mesh_pb2.Routing.NO_INTERFACE)
 msg.set_config.lora.tx_enabled=True
 sent=iface.localNode._sendAdmin(msg,wantResponse=True);ack(sent.id,mesh_pb2.Routing.BAD_REQUEST)
 msg.set_config.lora.tx_enabled=False;msg.set_config.lora.region=config_pb2.Config.LoRaConfig.EU_868
 sent=iface.localNode._sendAdmin(msg,wantResponse=True);ack(sent.id,mesh_pb2.Routing.BAD_REQUEST)
 old=len(messages);iface._startConfig()
 wait(lambda:any(r.HasField('config_complete_id') and r.config_complete_id==iface.configId for r in messages[old:]))
 assert iface.getMyNodeInfo()['user']['longName']=='ZT Demo'
 assert iface.localNode.localConfig.lora.region==config_pb2.Config.LoRaConfig.US
 assert not iface.localNode.localConfig.lora.tx_enabled
 assert iface.localNode.localConfig.lora.sx126x_rx_boosted_gain
 report.update(status='PASS',firmware=iface.metadata.firmware_version,handshake=True,owner_Public_LoRa_setters=True,local_text_and_ACK=True,remote_text_denied=True,unsupported_config_rejected=True,config_readback=True,protocol_records=len(messages))
finally:
 if iface:iface.close()
 faulthandler.cancel_dump_traceback_later()
 a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
