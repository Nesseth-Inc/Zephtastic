#!/usr/bin/env python3
"""Actual Meshtastic 2.7.11 owner/channel/LoRa vectors against M17 in memory."""
import argparse,hashlib,importlib.metadata,importlib.util,inspect,json,os,select,shlex,shutil,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[2];app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
client=module('m17_client',app/'tests/stock-private-client/client.py')
from meshtastic.node import Node
from meshtastic.protobuf import admin_pb2,channel_pb2,config_pb2
mesh,port=client.mesh_pb2,client.portnums_pb2
assert importlib.metadata.version('meshtastic')=='2.7.11'
files=[app/'src/Companion.h',app/'src/SettingsEdit.h',app/'src/NormalTxStartup.h',app/'tests/unchanged_settings_oracle.cpp',Path(__file__).resolve(),app/'tests/stock-private-client/client.py',proto/'admin.pb.h',proto/'channel.pb.h',proto/'config.pb.h']
def hashes():return {str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
before=hashes();report={'status':'FAIL','hardware':False,'RF':False,'keys':False,'client_version':'2.7.11','short_enums':a.short_enums,'checks':[]}
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
oracle=out/'unchanged-settings-oracle';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/unchanged_settings_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(oracle)]
commands.append(cmd);subprocess.run(cmd,check=True)

def admin_requests(stock,action,expected):
 start=len(stock.sent);action();selected=[]
 for raw in stock.sent[start:]:
  request=mesh.ToRadio.FromString(raw)
  if not request.HasField('packet') or request.packet.decoded.portnum!=port.ADMIN_APP:continue
  admin=admin_pb2.AdminMessage.FromString(request.packet.decoded.payload)
  if admin.WhichOneof('payload_variant')==expected:selected.append((raw,request,admin))
 check('actual Node API emitted one '+expected,len(selected)==1);return selected[0]
def fixture(model):
 stock=client.StockClient(0x12345678,0x87654321)
 stock.localNode.channels=[channel_pb2.Channel(index=i) for i in range(8)]
 primary=stock.localNode.channels[0];primary.role=channel_pb2.Channel.PRIMARY;primary.settings.psk=b'\x01';primary.settings.module_settings.SetInParent()
 lora=stock.localNode.localConfig.lora;lora.region=config_pb2.Config.LoRaConfig.UNSET;lora.use_preset=True
 lora.modem_preset=config_pb2.Config.LoRaConfig.LONG_FAST;lora.hop_limit=3;lora.channel_num=20;lora.sx126x_rx_boosted_gain=True;lora.tx_power=-9 if model==127 else 2;lora.tx_enabled=False
 vectors=[]
 vectors.append(('owner_long',*admin_requests(stock,lambda:stock.localNode.setOwner(long_name='ZephTastic'),'set_owner')))
 vectors.append(('owner_short',*admin_requests(stock,lambda:stock.localNode.setOwner(short_name='ZT'),'set_owner')))
 vectors.append(('channel_short',*admin_requests(stock,lambda:stock.localNode.writeChannel(0),'set_channel')))
 primary.settings.psk=bytes.fromhex('d4f1bb3a20290759f0bcffabcf4e6901')
 vectors.append(('channel_full',*admin_requests(stock,lambda:stock.localNode.writeChannel(0),'set_channel')))
 primary.settings.psk=b'\x01'
 vectors.append(('lora_legacy_disable',*admin_requests(stock,lambda:stock.localNode.writeConfig('lora'),'set_config')))
 vectors.append(('lora_legacy_disable_same',*admin_requests(stock,lambda:stock.localNode.writeConfig('lora'),'set_config')))
 lora.region=config_pb2.Config.LoRaConfig.US
 vectors.append(('lora_us_disable',*admin_requests(stock,lambda:stock.localNode.writeConfig('lora'),'set_config')))
 vectors.append(('lora_us_disable_same',*admin_requests(stock,lambda:stock.localNode.writeConfig('lora'),'set_config')))
 lora.tx_enabled=True
 vectors.append(('lora_us_enable',*admin_requests(stock,lambda:stock.localNode.writeConfig('lora'),'set_config')))
 vectors.append(('lora_us_enable_same',*admin_requests(stock,lambda:stock.localNode.writeConfig('lora'),'set_config')))
 lora.channel_num=0;lora.tx_power=0
 vectors.append(('lora_default_alias',*admin_requests(stock,lambda:stock.localNode.writeConfig('lora'),'set_config')))
 return vectors

transcripts=[]
for model in (81,127):
 vectors=fixture(model)
 for transport in ('usb','ble'):
  with (out/f'oracle-{model}-{transport}.stderr').open('w') as errors:
   process=subprocess.Popen([str(oracle),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=errors,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   previous=None
   try:
    for name,raw,request,admin in vectors:
     process.stdin.write(transport+' '+raw.hex()+'\n');process.stdin.flush();check(f'{model}/{transport}/{name} bounded response',bool(select.select([process.stdout],[],[],5)[0]))
     state=json.loads(process.stdout.readline());records=[mesh.FromRadio.FromString(bytes.fromhex(value)) for value in state['records_hex']]
     routes=[record.packet for record in records if record.HasField('packet') and record.packet.decoded.portnum==port.ROUTING_APP]
     check(f'{model}/{transport}/{name} correlated success',len(routes)==1 and routes[0].decoded.request_id==request.packet.id and mesh.Routing.FromString(routes[0].decoded.payload).error_reason==mesh.Routing.NONE)
     check(f'{model}/{transport}/{name} no RF send',state['sends']==0)
     if name in ('owner_long','owner_short','channel_short','channel_full'):
      check(f'{model}/{transport}/{name} inert initial no-op',state['writes']==0 and state['saves']==0 and state['starts']==0 and state['stops']==0)
     if name=='lora_legacy_disable':check(f'{model}/{transport} unspecified intent becomes explicit',state['writes']==1 and state['tx_intent']==1)
     if name=='lora_legacy_disable_same':check(f'{model}/{transport} explicit disable echo inert',state['writes']==1 and previous and state['saves']==previous['saves'])
     if name=='lora_us_disable':check(f'{model}/{transport} changed US save starts RX disarmed',state['writes']==2 and state['receiving'] and not state['armed'])
     if name=='lora_us_disable_same':check(f'{model}/{transport} unchanged US disable inert',previous and state['writes']==2 and state['saves']==previous['saves'] and state['starts']==previous['starts'] and state['stops']==previous['stops'])
     if name=='lora_us_enable':check(f'{model}/{transport} changed enable arms',state['writes']==3 and state['receiving'] and state['armed'])
     if name in ('lora_us_enable_same','lora_default_alias'):check(f'{model}/{transport}/{name} enabled echo inert',previous and state['writes']==3 and state['saves']==previous['saves'] and state['starts']==previous['starts'] and state['stops']==previous['stops'] and state['armed'])
     transcripts.append({'model':model,'transport':transport,'name':name,'admin_hex':request.packet.decoded.payload.hex(),'state':state});previous=state
   finally:
    process.stdin.close();process.wait(timeout=5)
   check(f'{model}/{transport} oracle exit',process.returncode==0)
check('stock Node implementation hash recorded',bool(hashlib.sha256(inspect.getsource(Node).encode()).hexdigest()))
check('source stable',hashes()==before);report.update(status='PASS',source_sha256=before,node_api_sha256=hashlib.sha256(inspect.getsource(Node).encode()).hexdigest(),vectors=11,board_transport_cases=4)
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(report['checks']),'checks; actual Meshtastic 2.7.11 Node APIs, memory-only USB/BLE framing')
