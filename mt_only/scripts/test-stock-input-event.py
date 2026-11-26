#!/usr/bin/env python3
"""Meshtastic 2.7.11 send_input_event API/parser differential; no hardware/RF."""
import argparse,hashlib,importlib.metadata,importlib.util,inspect,json,os,re,select,shlex,shutil,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--stock-root',required=True,type=Path);p.add_argument('--short-enums',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[2];app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
client=module('m19_client',app/'tests/stock-private-client/client.py')
from meshtastic.node import Node
from meshtastic.protobuf import admin_pb2,mesh_pb2,portnums_pb2
assert importlib.metadata.version('meshtastic')=='2.7.11'
stock=a.stock_root.resolve()
files=[app/'src/Companion.h',app/'src/ui/Model.h',app/'src/ui/Display.h',app/'src/ui/Display.cpp',
       app/'tests/input_event_oracle.cpp',Path(__file__).resolve(),app/'tests/stock-private-client/client.py',
       proto/'admin.pb.h',stock/'src/input/InputBroker.h',stock/'src/input/InputBroker.cpp',stock/'src/modules/AdminModule.cpp']
def hashes():return {str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
before=hashes();report={'status':'FAIL','hardware':False,'RF':False,'keys':False,'client_version':'2.7.11','short_enums':a.short_enums,'checks':[]}
def check(name,value):
 report['checks'].append({'name':name,'status':'PASS' if value else 'FAIL'})
 if not value:raise AssertionError(name)
expected_hashes={
 str(stock/'src/input/InputBroker.h'):'95fc646df33c8e3ee95bc25a52ad647b7307030c32669b70044e3424e039ab58',
 str(stock/'src/input/InputBroker.cpp'):'ca654c98569119dc7e4c8203b77a4db55bee9070959eadd82dbb0357b9b58a2c',
 str(stock/'src/modules/AdminModule.cpp'):'ee758ed26686b718f592d7a9df048cefecfca13a2c7a5d01fd11192b0de73a28',
 str(proto/'admin.pb.h'):'5dd83418156d92c75b15a14dd7dd44e2773d3526eddab871a632a4029104848a',
}
for path,digest in expected_hashes.items():check('pinned hash '+Path(path).name,before[path]==digest)
stock_oid=subprocess.check_output(['git','-C',str(stock),'rev-parse','HEAD'],text=True).strip()
check('pinned stock Git OID',stock_oid=='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c')
node_api_sha256=hashlib.sha256(inspect.getsource(Node).encode()).hexdigest()
check('pinned Meshtastic Node API hash',node_api_sha256=='fd5f81c5bc41a782c8811c6b222dbaa0975ad0b43cf7d86ba906a4f34065f2b1')
(out/'meshtastic').mkdir(exist_ok=True)
for header in proto.glob('*.pb.h'):shutil.copyfile(header,out/'meshtastic'/header.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb'),'-DMT_HOST_OPENSSL=1']
if a.short_enums:flags+=['-fshort-enums']
cc=shlex.split(os.environ.get('CC','gcc'));cxx=shlex.split(os.environ.get('CXX','g++'));commands=[];objects=[]
for source in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/name for name in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(source.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(source),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
oracle=out/'input-event-oracle';cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/input_event_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-lcrypto','-o',str(oracle)]
commands.append(cmd);subprocess.run(cmd,check=True)

def request(stock_client,code,key=0,x=0,y=0):
 start=len(stock_client.sent);message=admin_pb2.AdminMessage();event=message.send_input_event
 event.event_code=code;event.kb_char=key;event.touch_x=x;event.touch_y=y
 sent=stock_client.localNode._sendAdmin(message,wantResponse=True);selected=[]
 for raw in stock_client.sent[start:]:
  value=mesh_pb2.ToRadio.FromString(raw)
  if not value.HasField('packet') or value.packet.decoded.portnum!=portnums_pb2.ADMIN_APP:continue
  admin=admin_pb2.AdminMessage.FromString(value.packet.decoded.payload)
  if admin.WhichOneof('payload_variant')=='send_input_event':selected.append((raw,value,admin))
 check('actual Node._sendAdmin emitted one send_input_event',len(selected)==1)
 check('actual Node._sendAdmin retained local PKI hint',selected[0][1].packet.pki_encrypted)
 return selected[0]
def exchange(process,transport,raw):
 process.stdin.write(transport+' '+raw.hex()+'\n');process.stdin.flush();check(transport+' bounded response',bool(select.select([process.stdout],[],[],5)[0]));return json.loads(process.stdout.readline())
def routes(records,request_packet):
 values=[]
 for record in records:
  if record.HasField('packet') and record.packet.decoded.portnum==portnums_pb2.ROUTING_APP and record.packet.decoded.request_id==request_packet.packet.id:
   values.append(mesh_pb2.Routing.FromString(record.packet.decoded.payload))
 return values
def notifications(records,request_packet):
 return [r.clientNotification.message for r in records if r.HasField('clientNotification') and r.clientNotification.reply_id==request_packet.packet.id]

supported=(10,17,18,19,20,27,28);transcripts=[]
for model in (81,127):
 for transport in ('usb','ble'):
  stock_client=client.StockClient(0x12345678,0x87654321)
  with (out/f'oracle-{model}-{transport}.stderr').open('w') as errors:
   process=subprocess.Popen([str(oracle),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=errors,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   try:
    last_calls=0
    for code in supported:
     raw,request_packet,admin=request(stock_client,code);state=exchange(process,transport,raw)
     records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']]
     result=routes(records,request_packet);expected=mesh_pb2.Routing.NONE if model==127 else mesh_pb2.Routing.NO_INTERFACE
     check(f'{model}/{transport}/{code} correlated result',len(result)==1 and result[0].error_reason==expected)
     if model==81:check(f'{model}/{transport}/{code} truthful headless notice',any('Display input unavailable' in message for message in notifications(records,request_packet)))
     stock_client.accept(state['records_hex'])
     check(f'{model}/{transport}/{code} real parser consumed response',all(r.ByteSize() for r in records))
     check(f'{model}/{transport}/{code} exact dispatch',state['ui_calls']==last_calls+1 and state['last_code']==code)
     check(f'{model}/{transport}/{code} no persistence or RF',not state['writes'] and not state['saves'] and not state['sends'] and state['free']==4 and not state['armed'])
     last_calls=state['ui_calls']
    for label,values in [('cancel',(24,0,0,0)),('keyboard',(10,65,0,0)),('touch',(10,0,1,2)),('overflow',(266,0,0,0))]:
     raw,request_packet,admin=request(stock_client,*values);state=exchange(process,transport,raw)
     records=[mesh_pb2.FromRadio.FromString(bytes.fromhex(v)) for v in state['records_hex']];result=routes(records,request_packet)
     check(f'{model}/{transport}/{label} refused',len(result)==1 and result[0].error_reason==mesh_pb2.Routing.BAD_REQUEST and state['ui_calls']==last_calls)
     stock_client.accept(state['records_hex']);check(f'{model}/{transport}/{label} no mutation',not state['writes'] and not state['saves'] and not state['sends'] and state['free']==4 and not state['armed'])
    transcripts.append({'model':model,'transport':transport,'supported_codes':list(supported),'final_ui_calls':last_calls})
   finally:
    process.stdin.close();process.wait(timeout=5)
   check(f'{model}/{transport} oracle exit',process.returncode==0)
enum_source=(stock/'src/input/InputBroker.h').read_text()
enum_body=re.search(r'enum\s+input_broker_event\s*\{(.*?)\};',enum_source,re.S).group(1)
enum_values={};current=-1
for item in re.sub(r'//[^\n]*','',enum_body).split(','):
 item=item.strip()
 if not item:continue
 name,separator,value=item.partition('=')
 current=int(value.strip(),0) if separator else current+1
 enum_values[name.strip()]=current
check('pinned event enum parsed exact values',enum_values=={
 'INPUT_BROKER_NONE':0,'INPUT_BROKER_SELECT':10,'INPUT_BROKER_SELECT_LONG':11,
 'INPUT_BROKER_UP_LONG':12,'INPUT_BROKER_DOWN_LONG':13,'INPUT_BROKER_UP':17,
 'INPUT_BROKER_DOWN':18,'INPUT_BROKER_LEFT':19,'INPUT_BROKER_RIGHT':20,
 'INPUT_BROKER_CANCEL':24,'INPUT_BROKER_BACK':27,'INPUT_BROKER_USER_PRESS':28,
 'INPUT_BROKER_ALT_PRESS':29,'INPUT_BROKER_ALT_LONG':30,'INPUT_BROKER_FACTORY_RST':0x9a,
 'INPUT_BROKER_SHUTDOWN':0x9b,'INPUT_BROKER_GPS_TOGGLE':0x9e,
 'INPUT_BROKER_PRIVACY_TOGGLE':0x9f,'INPUT_BROKER_SEND_PING':0xaf,
 'INPUT_BROKER_FN_F1':0xf1,'INPUT_BROKER_FN_F2':0xf2,'INPUT_BROKER_FN_F3':0xf3,
 'INPUT_BROKER_FN_F4':0xf4,'INPUT_BROKER_FN_F5':0xf5,'INPUT_BROKER_MATRIXKEY':0xfe,
 'INPUT_BROKER_ANYKEY':0xff})
check('stock wake consumption source present','screenWasOff' in (stock/'src/input/InputBroker.cpp').read_text() and 'if (screen && screenWasOff)' in (stock/'src/input/InputBroker.cpp').read_text())
check('source stable',hashes()==before)
report.update(status='PASS',source_sha256=before,node_api_sha256=node_api_sha256,stock_git_oid=stock_oid,board_transport_cases=4,supported_codes=list(supported))
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(report['checks']),'checks; actual Meshtastic 2.7.11 AdminMessage + Node._sendAdmin + FromRadio parser, memory-only USB/BLE')
