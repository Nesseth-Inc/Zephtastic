#!/usr/bin/env python3
"""Actual Meshtastic 2.7.11 traceroute API/callback through the ZT host bridge."""
import argparse,hashlib,importlib.metadata,inspect,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
from unittest.mock import patch
from meshtastic import mesh_interface
from meshtastic.mesh_interface import MeshInterface
from meshtastic.protobuf import mesh_pb2,portnums_pb2

p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-root',required=True,type=Path);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);app=Path(__file__).resolve().parents[1]
vendor=app.parent/'zephcore/adapters/bench/vendor';mono=app.parent/'zephcore/lib/monocypher';proto=app.parent/'zephcore/bench/upstream-differential/source'
assert importlib.metadata.version('meshtastic')=='2.7.11'
assert (a.stock_root/'.git').exists() and subprocess.check_output(['git','-C',str(a.stock_root),'rev-parse','HEAD'],text=True).strip()=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ['pb_common.c','pb_encode.c','pb_decode.c'])]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
bridge=out/'traceroute-client-bridge';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/traceroute_client_bridge.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(bridge)]
subprocess.run(cmd,check=True);commands.append(cmd)

class Stock(MeshInterface):
 def __init__(self):
  super().__init__(timeout=1);self.currentPacketId=0x4d100000;self.myInfo=mesh_pb2.MyNodeInfo(my_node_num=0x12345678)
  self.localNode.nodeNum=0x12345678;self.localNode.localConfig.lora.hop_limit=3
  self.nodesByNum={0x12345678:{'num':0x12345678,'user':{'id':'!12345678'}},0x87654321:{'num':0x87654321,'user':{'id':'!87654321'}}}
  self.nodes={v['user']['id']:v for v in self.nodesByNum.values()};self.isConnected.set();self.sent=[]
 def _generatePacketId(self):
  with patch.object(mesh_interface.random,'randint',return_value=0x134400):return super()._generatePacketId()
 def _sendToRadio(self,message):self.sent.append(message.SerializeToString(deterministic=True))

client=Stock()
with patch.object(client,'waitForTraceRoute',return_value=None):client.sendTraceRoute(0x87654321,3)
assert len(client.sent)==1;request=mesh_pb2.ToRadio.FromString(client.sent[0]);packet=request.packet
assert packet.to==0x87654321 and packet.channel==0 and packet.hop_limit==3 and not packet.want_ack
assert packet.decoded.portnum==portnums_pb2.TRACEROUTE_APP and packet.decoded.want_response and packet.decoded.payload==b''
run=[str(bridge),client.sent[0].hex()];commands.append(run)
proc=subprocess.run(run,text=True,capture_output=True,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
records=[bytes.fromhex(line) for line in proc.stdout.splitlines() if line.strip()];assert len(records)==1
reply=mesh_pb2.FromRadio.FromString(records[0]);rp=reply.packet;route=mesh_pb2.RouteDiscovery.FromString(rp.decoded.payload)
assert getattr(rp,'from')==0x87654321 and rp.to==0x12345678 and rp.decoded.request_id==packet.id
assert list(route.route)==[] and list(route.snr_towards)==[-40]
assert list(route.route_back)==[] and list(route.snr_back)==[-48]
# A successful Routing ACK is deliberately ignored and must leave the handler.
ack=mesh_pb2.FromRadio();setattr(ack.packet,'from',0x87654321);ack.packet.to=0x12345678;ack.packet.id=0x700
ack.packet.decoded.portnum=portnums_pb2.ROUTING_APP;ack.packet.decoded.request_id=packet.id
ack.packet.decoded.payload=mesh_pb2.Routing(error_reason=0).SerializeToString()
with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()),patch.object(mesh_interface.pub,'sendMessage',return_value=None):
 client._handleFromRadio(ack.SerializeToString());assert packet.id in client.responseHandlers
 client._handleFromRadio(records[0])
assert packet.id not in client.responseHandlers and client._acknowledgment.receivedTraceRoute
transit=Stock()
with patch.object(transit,'waitForTraceRoute',return_value=None):transit.sendTraceRoute(0x87654321,3)
transit_request=mesh_pb2.ToRadio.FromString(transit.sent[0]);transit_run=[str(bridge),transit.sent[0].hex(),'transit'];commands.append(transit_run)
transit_proc=subprocess.run(transit_run,text=True,capture_output=True,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
transit_records=[bytes.fromhex(line) for line in transit_proc.stdout.splitlines() if line.strip()];assert len(transit_records)==1
transit_reply=mesh_pb2.FromRadio.FromString(transit_records[0]);transit_route=mesh_pb2.RouteDiscovery.FromString(transit_reply.packet.decoded.payload)
assert transit_reply.packet.decoded.request_id==transit_request.packet.id
assert list(transit_route.route)==[0x55667788] and list(transit_route.snr_towards)==[-20,-40]
assert list(transit_route.route_back)==[0x55667788] and list(transit_route.snr_back)==[-28,-48]
with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()),patch.object(mesh_interface.pub,'sendMessage',return_value=None):
 transit._handleFromRadio(transit_records[0])
assert transit_request.packet.id not in transit.responseHandlers and transit._acknowledgment.receivedTraceRoute
asymmetric=Stock()
with patch.object(asymmetric,'waitForTraceRoute',return_value=None):asymmetric.sendTraceRoute(0x87654321,3)
asym_request=mesh_pb2.ToRadio.FromString(asymmetric.sent[0]);asym_run=[str(bridge),asymmetric.sent[0].hex(),'asymmetric'];commands.append(asym_run)
asym_proc=subprocess.run(asym_run,text=True,capture_output=True,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
asym_records=[bytes.fromhex(line) for line in asym_proc.stdout.splitlines() if line.strip()];assert len(asym_records)==1
asym_reply=mesh_pb2.FromRadio.FromString(asym_records[0]);asym_route=mesh_pb2.RouteDiscovery.FromString(asym_reply.packet.decoded.payload)
assert asym_reply.packet.decoded.request_id==asym_request.packet.id
assert list(asym_route.route)==[0x11112222] and list(asym_route.snr_towards)==[4,12]
assert list(asym_route.route_back)==[0x33334444,0xffffffff] and list(asym_route.snr_back)==[-8,-128,127]
with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()),patch.object(mesh_interface.pub,'sendMessage',return_value=None):
 asymmetric._handleFromRadio(asym_records[0])
assert asym_request.packet.id not in asymmetric.responseHandlers and asymmetric._acknowledgment.receivedTraceRoute
checks=['actual 2.7.11 sendTraceRoute request crossed the production Companion/Service/Endpoint path at two virtual endpoints with no relay',
        'a separate actual request crossed production origin, relay, destination, return-relay and origin paths with integer-dB-derived q4 SNR',
        'actual callback decoded an asymmetric production-normalized response injected into the origin/client path',
        'successful Routing ACK did not masquerade as traceroute completion']
sources=[Path(__file__).resolve(),app/'tests/traceroute_client_bridge.cpp',app/'src/Companion.h',app/'src/Service.h',app/'src/Endpoint.h',app/'src/TraceRoute.h',vendor/'MtCodec.h',vendor/'RoutingCore.h',Path(inspect.getsourcefile(mesh_interface)).resolve()]
result={'status':'PASS','hardware':False,'virtual_radio':True,'client_version':'2.7.11','stock_firmware_git_sha1':'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','request_id':packet.id,'checks':checks,'cases':4,'request_toradio_sha256':hashlib.sha256(client.sent[0]).hexdigest(),'direct_response_fromradio_sha256':hashlib.sha256(records[0]).hexdigest(),'transit_response_fromradio_sha256':hashlib.sha256(transit_records[0]).hexdigest(),'asymmetric_response_fromradio_sha256':hashlib.sha256(asym_records[0]).hexdigest(),'source_sha256':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sources}}
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
