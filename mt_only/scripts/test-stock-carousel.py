#!/usr/bin/env python3
"""Meshtastic 2.7.11 DisplayConfig carousel differential; no hardware/RF."""
import argparse,hashlib,importlib.metadata,importlib.util,json,os,select,shlex,shutil,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-root',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[2];app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
client=module('m20_client',app/'tests/stock-private-client/client.py')
import meshtastic.node as node_module
from meshtastic.node import Node
from meshtastic.protobuf import admin_pb2,config_pb2,mesh_pb2,portnums_pb2
assert importlib.metadata.version('meshtastic')=='2.7.11'
stock=a.stock_root.resolve()
node_path=Path(node_module.__file__).resolve()
files=[app/'src/Companion.h',app/'src/Config.h',app/'src/ui/Display.cpp',app/'tests/carousel_oracle.cpp',Path(__file__).resolve(),node_path,
 app/'tests/stock-private-client/client.py',proto/'config.pb.h',stock/'src/graphics/Screen.cpp',stock/'src/modules/AdminModule.cpp']
def hashes():return {str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
before=hashes();report={'status':'FAIL','hardware':False,'RF':False,'keys':False,'client_version':'2.7.11','short_enums':a.short_enums,'checks':[]}
def check(name,value):
 report['checks'].append({'name':name,'status':'PASS' if value else 'FAIL'})
 if not value:raise AssertionError(name)
expected={str(stock/'src/graphics/Screen.cpp'):'5b66c4f5327cabd8d9ad50217e2d117ec72ab8bea0cd54b994259d6c985be78f',
 str(stock/'src/modules/AdminModule.cpp'):'ee758ed26686b718f592d7a9df048cefecfca13a2c7a5d01fd11192b0de73a28',
 str(proto/'config.pb.h'):'627776cc446d3f293e2e5ab6f1fc4263ec6ef50dec67f0a2b9fcda04b9dc795c'}
for path,digest in expected.items():check('pinned hash '+Path(path).name,before[path]==digest)
check('pinned stock Git OID',subprocess.check_output(['git','-C',str(stock),'rev-parse','HEAD'],text=True).strip()=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c')
node_file_sha256=hashlib.sha256(node_path.read_bytes()).hexdigest()
check('exact installed Meshtastic 2.7.11 Node source',node_file_sha256=='74f0c5418458225258ca9455070ea3a559f6db09974170a0baf07fbf72b04136')
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-O2','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
oracle=out/'carousel-oracle';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/carousel_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(oracle)]
commands.append(cmd);subprocess.run(cmd,check=True)

def request(stock_client,timeout,flip,carousel):
 d=stock_client.localNode.localConfig.display;d.screen_on_secs=timeout;d.flip_screen=flip;d.auto_screen_carousel_secs=carousel
 start=len(stock_client.sent);stock_client.localNode.writeConfig('display');selected=[]
 for raw in stock_client.sent[start:]:
  value=mesh_pb2.ToRadio.FromString(raw)
  if value.HasField('packet') and value.packet.decoded.portnum==portnums_pb2.ADMIN_APP:
   admin=admin_pb2.AdminMessage.FromString(value.packet.decoded.payload)
   if admin.WhichOneof('payload_variant')=='set_config' and admin.set_config.WhichOneof('payload_variant')=='display':selected.append((raw,value))
 check('actual Node.writeConfig emitted one display request',len(selected)==1);return selected[0]
def get_request(stock_client):
 message=admin_pb2.AdminMessage();message.get_config_request=admin_pb2.AdminMessage.DISPLAY_CONFIG
 start=len(stock_client.sent);stock_client.localNode._sendAdmin(message,wantResponse=True);selected=[]
 for raw in stock_client.sent[start:]:
  value=mesh_pb2.ToRadio.FromString(raw)
  if value.HasField('packet') and value.packet.decoded.portnum==portnums_pb2.ADMIN_APP:selected.append((raw,value))
 check('actual Node._sendAdmin emitted one display getter',len(selected)==1);return selected[0]
def exchange(process,transport,raw):
 process.stdin.write(transport+' '+raw.hex()+'\n');process.stdin.flush();check(transport+' bounded response',bool(select.select([process.stdout],[],[],5)[0]));return json.loads(process.stdout.readline())
def decoded(state):return [mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']]
def route(records,request):
 values=[]
 for record in records:
  if record.HasField('packet') and record.packet.decoded.portnum==portnums_pb2.ROUTING_APP and record.packet.decoded.request_id==request.packet.id:values.append(mesh_pb2.Routing.FromString(record.packet.decoded.payload))
 return values

transcripts=[]
for model in (81,127):
 for transport in ('usb','ble'):
  stock_client=client.StockClient(0x12345678,0x87654321)
  with (out/f'oracle-{model}-{transport}.stderr').open('w') as errors:
   process=subprocess.Popen([str(oracle),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=errors,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   try:
    raw,req=request(stock_client,15,False,0);state=exchange(process,transport,raw);records=decoded(state);result=route(records,req)
    check(f'{model}/{transport} default echo',len(result)==1 and result[0].error_reason==mesh_pb2.Routing.NONE and not state['writes'])
    raw,req=request(stock_client,60,True,30);state=exchange(process,transport,raw);records=decoded(state);result=route(records,req)
    expected_result=mesh_pb2.Routing.NONE if model==127 else mesh_pb2.Routing.BAD_REQUEST
    check(f'{model}/{transport} bounded activation',len(result)==1 and result[0].error_reason==expected_result)
    check(f'{model}/{transport} exact persisted state',(state['timeout'],state['flip'],state['carousel'])==((60,True,30) if model==127 else (15,False,0)))
    raw,req=get_request(stock_client);state=exchange(process,transport,raw);records=decoded(state)
    replies=[admin_pb2.AdminMessage.FromString(r.packet.decoded.payload) for r in records if r.HasField('packet') and r.packet.decoded.portnum==portnums_pb2.ADMIN_APP and r.packet.decoded.request_id==req.packet.id]
    check(f'{model}/{transport} FromRadio getter parse',len(replies)==1 and replies[0].get_config_response.WhichOneof('payload_variant')=='display')
    d=replies[0].get_config_response.display;check(f'{model}/{transport} getter agrees',(d.screen_on_secs,d.flip_screen,d.auto_screen_carousel_secs)==((60,True,30) if model==127 else (15,False,0)))
    check(f'{model}/{transport} no RF',state['sends']==0);transcripts.append({'model':model,'transport':transport,'state':state})
   finally:
    process.stdin.close();process.wait(timeout=5)
   check(f'{model}/{transport} oracle exit',process.returncode==0)
check('stock source contains carousel field use','auto_screen_carousel_secs > 0' in (stock/'src/graphics/Screen.cpp').read_text())
check('source stable',hashes()==before)
report.update(status='PASS',source_sha256=before,node_file=str(node_path),node_file_sha256=node_file_sha256,board_transport_cases=4)
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(report['checks']),'checks; actual Meshtastic 2.7.11 Node.writeConfig/_sendAdmin/FromRadio, memory-only USB/BLE')
