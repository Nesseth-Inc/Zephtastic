#!/usr/bin/env python3
"""Actual Python 2.7.11 Device saves/getters against a native memory-backed ZT.

No hardware transport, RF, keys or firmware image is used. Optional --stock
checks the pinned upstream clamp statements; it does not execute stock firmware.
"""
import argparse
import hashlib
import importlib.util
import inspect
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import subprocess
from unittest.mock import patch

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',required=True,type=Path)
p.add_argument('--stock',type=Path)
p.add_argument('--short-enums',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2];app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
client=module('client',app/'tests/stock-private-client/client.py')
provenance=module('provenance',app/'tests/stock-private-client/provenance.py')
from meshtastic.protobuf import admin_pb2,config_pb2
from meshtastic.node import Node
mesh,port=client.mesh_pb2,client.portnums_pb2
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
files=[f for directory in (app/'src',vendor,mono,proto) for f in directory.rglob('*') if f.is_file() and f.suffix in ('.h','.cpp','.c')]
files += [Path(__file__).resolve(),app/'tests/device_config_fixture.h',app/'tests/device_config_oracle.cpp',app/'tests/stock-private-client/client.py',app/'tests/stock-private-client/provenance.py']
def hashes():return {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(files)}
before=hashes();report=dict(status='FAIL',hardware=False,RF=False,keys=False,ASAN_UBSAN=True,short_enums=a.short_enums,
 **provenance.source_provenance(root),python=client.provenance(),
 node_api_sha256=hashlib.sha256(inspect.getsource(Node).encode()).hexdigest(),checks=[])
def check(name,value):
 report['checks'].append(dict(name=name,status='PASS' if value else 'FAIL'))
 if not value:raise AssertionError(name)
commands=[];transcripts=[]
try:
 if a.stock:
  stock=a.stock.resolve();revision='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
  pinned={'src/mesh/Default.h':'9874226a4ee5719ade291a4e541f3475f87973edd8497deb72e193fc6970aa1b','src/modules/AdminModule.cpp':'ee758ed26686b718f592d7a9df048cefecfca13a2c7a5d01fd11192b0de73a28'}
  check('reference captured files hash-pinned',all(hashlib.sha256((stock/name).read_bytes()).hexdigest()==digest for name,digest in pinned.items()))
  defaults=(stock/'src/mesh/Default.h').read_text();admin=(stock/'src/modules/AdminModule.cpp').read_text()
  check('stock minimum is one hour','#define min_node_info_broadcast_secs 60 * 60' in defaults)
  check('stock actual setter clamps low values','if (config.device.node_info_broadcast_secs < min_node_info_broadcast_secs)' in admin and 'config.device.node_info_broadcast_secs = min_node_info_broadcast_secs;' in admin)
  report['stock_reference']=dict(revision=revision,comparison='source statements plus fixed protocol vectors; no stock runtime execution',sha256={str(f.relative_to(stock)):hashlib.sha256(f.read_bytes()).hexdigest() for f in (stock/'src/mesh/Default.h',stock/'src/modules/AdminModule.cpp')})
 (out/'meshtastic').mkdir(exist_ok=True)
 for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
 flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
 if a.short_enums:flags+=['-fshort-enums']
 cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
 objects=[]
 for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
  obj=out/(src.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(src),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
 executable=out/'device-config-oracle'
 cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/device_config_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,'-o',str(executable)]
 commands.append(cmd);subprocess.run(cmd,check=True)
 for model in (81,127):
  for transport in ('usb','ble'):
   label=f'{model}/{transport}';opposite='ble' if transport=='usb' else 'usb'
   actual=client.StockClient(0x12345678,0x87654321);peer=client.StockClient(0x12345678,0x87654321)
   with (out/f'oracle-{model}-{transport}.stderr').open('w') as errors:
    process=subprocess.Popen([str(executable),str(model)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1,stderr=errors,
     env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
    def exchange(command):
     process.stdin.write(command+'\n');process.stdin.flush()
     check(label+' bounded oracle reply',bool(select.select([process.stdout],[],[],5)[0]))
     row=json.loads(process.stdout.readline());transcripts.append(dict(board=model,transport=transport,command=command,response=row))
     check(label+' no radio or TX activation',not row['radio_sends'] and not row['radio_starts'] and not row['tx_armed'])
     return row
    def snapshot(who,via):
     with patch.object(client.mesh_interface.random,'randint',return_value=71):who._startConfig()
     row=exchange(via+' '+who.sent[-1].hex())
     who.accept(row['records_hex'])
     return row
    def request(who,via,action):
     count=len(who.sent);action();pending=who.sent[count:]
     check(label+' bounded actual API request count',len(pending) in (1,2))
     if len(pending)==2:
      first=mesh.ToRadio.FromString(pending[0]).packet
      admin=admin_pb2.AdminMessage.FromString(first.decoded.payload)
      check(label+' real transaction session-config preflight',admin.HasField('get_config_request') and admin.get_config_request==admin_pb2.AdminMessage.SESSIONKEY_CONFIG)
     for raw in pending:
      packet=mesh.ToRadio.FromString(raw).packet
      check(label+' actual local admin PKI hint',packet.to==0x12345678 and packet.pki_encrypted and packet.decoded.portnum==port.ADMIN_APP)
      row=exchange(via+' '+raw.hex());records=[mesh.FromRadio.FromString(bytes.fromhex(r)) for r in row['records_hex']]
      check(label+' first correlated queue',bool(records) and records[0].HasField('queueStatus') and records[0].queueStatus.mesh_packet_id==packet.id)
      packets=[r.packet for r in records if r.HasField('packet')]
      check(label+' one correlated fresh local result',len(packets)==1 and packets[0].decoded.request_id==packet.id and not packets[0].pki_encrypted and not packets[0].public_key)
      callbacks=[];who._addResponseHandler(packet.id,callbacks.append,ackPermitted=True);who.accept(row['records_hex'])
      check(label+' stock response callback',len(callbacks)==1)
      result=packets[0]
      if result.decoded.portnum==port.ROUTING_APP:
       row['error']=mesh.Routing.FromString(result.decoded.payload).error_reason
      else:
       check(label+' admin reply app',result.decoded.portnum==port.ADMIN_APP)
       row['admin']=admin_pb2.AdminMessage.FromString(result.decoded.payload)
     return row
    def write(seconds,expected=mesh.Routing.NONE):
     actual.localNode.localConfig.device.node_info_broadcast_secs=seconds
     row=request(actual,transport,lambda:actual.localNode.writeConfig('device'));check(label+' expected save result',row['error']==expected);return row
    def read(who=peer,via=opposite):
     row=request(who,via,lambda:who.localNode.requestConfig(admin_pb2.AdminMessage.DEVICE_CONFIG))
     check(label+' actual device getter',row['admin'].HasField('get_config_response') and row['admin'].get_config_response.HasField('device'))
     return row['admin'].get_config_response.device
    try:
     row=snapshot(actual,transport);snapshot(peer,opposite)
     initial=config_pb2.Config.DeviceConfig();initial.CopyFrom(actual.localNode.localConfig.device)
     check(label+' handshake actual persisted default',initial.node_info_broadcast_secs==10800 and initial.role==config_pb2.Config.DeviceConfig.CLIENT_MUTE and initial.serial_enabled and initial.led_heartbeat_disabled)
     check(label+' getter equals handshake',read()==initial)
     same=write(10800);check(label+' no-op no writes',same['writes']==0)
     prior=0
     for seconds,want in ((0,3600),(1,3600),(3599,3600),(3600,3600),(7200,7200),(86400,86400),(86401,86401),(604800,604800),(2147483,2147483)):
      row=write(seconds);check(label+' effective clamp readback',read().node_info_broadcast_secs==want and row['interval']==want)
      expected=prior+(seconds in (0,7200,86400,86401,604800,2147483));check(label+' equivalent effective value avoids write',row['writes']==expected);prior=row['writes']
     for seconds in (2147484,0x7fffffff,0xffffffff):
      row=write(seconds,mesh.Routing.BAD_REQUEST);check(label+' rejected value preserves stored record',read().node_info_broadcast_secs==2147483 and row['writes']==prior)
     # CLIENT is a supported complete replacement; preserve the current interval explicitly.
     actual.localNode.localConfig.device.CopyFrom(initial)
     actual.localNode.localConfig.device.role=config_pb2.Config.DeviceConfig.CLIENT
     actual.localNode.localConfig.device.node_info_broadcast_secs=2147483
     row=request(actual,transport,lambda:actual.localNode.writeConfig('device'))
     check(label+' supported CLIENT save',row['error']==mesh.Routing.NONE and row['writes']==prior+1)
     prior=row['writes'];current=read()
     check(label+' supported CLIENT opposite getter',current.role==config_pb2.Config.DeviceConfig.CLIENT and current.node_info_broadcast_secs==2147483)
     # Later invalid replacements must preserve this accepted role; the existing journal restart proves durability.
     initial.role=config_pb2.Config.DeviceConfig.CLIENT
     for field,value in (('serial_enabled',False),('led_heartbeat_disabled',False),('button_gpio',1),('buzzer_gpio',1),('rebroadcast_mode',1),('double_tap_as_button_press',True),('is_managed',True),('disable_triple_click',True),('tzdef','UTC0'),('tzdef','\0UTC0'),('buzzer_mode',1),('role',99),('role',-1),('rebroadcast_mode',99),('rebroadcast_mode',-1),('buzzer_mode',99),('buzzer_mode',-1),('role',257),('role',65537),('rebroadcast_mode',256),('rebroadcast_mode',65536),('buzzer_mode',256),('buzzer_mode',65536)):
      actual.localNode.localConfig.device.CopyFrom(initial);setattr(actual.localNode.localConfig.device,field,value)
      row=request(actual,transport,lambda:actual.localNode.writeConfig('device'));check(label+' rejects '+field,row['error']==mesh.Routing.BAD_REQUEST and row['writes']==prior and read().node_info_broadcast_secs==2147483)
     actual.localNode.localConfig.device.CopyFrom(initial)
     row=request(actual,transport,actual.localNode.beginSettingsTransaction);check(label+' actual begin accepted',row['error']==0 and row['editing'])
     row=write(7200);check(label+' live batch has no write',row['writes']==prior and row['dirty'] and read().node_info_broadcast_secs==7200)
     row=write(7200);check(label+' batch no-op no extra write',row['writes']==prior)
     # Other transport commits the shared live editor using the real Node API.
     row=request(peer,opposite,peer.localNode.commitSettingsTransaction);check(label+' cross-transport one verified commit',row['error']==0 and not row['editing'] and row['writes']==prior+1)
     row=exchange('restart');check(label+' journal-only restart retains interval',row['interval']==7200 and not row['tx_armed'])
     snapshot(actual,transport);current=read();check(label+' stock model refreshed after restart',actual.localNode.localConfig.device.node_info_broadcast_secs==7200 and current.node_info_broadcast_secs==7200)
     check(label+' journal-only restart retains supported CLIENT',actual.localNode.localConfig.device.role==config_pb2.Config.DeviceConfig.CLIENT and current.role==config_pb2.Config.DeviceConfig.CLIENT and row['writes']==prior+1)
    finally:
     process.stdin.close()
     try:process.wait(timeout=5)
     except subprocess.TimeoutExpired:process.kill();process.wait();raise
    check(label+' oracle exited successfully',process.returncode==0)
 check('source and fixtures stable',hashes()==before);report['source_sha256']=before;report['status']='PASS'
finally:
 # Protobuf objects live only in parsed assertions, not saved transcript rows.
 for row in transcripts:row['response'].pop('admin',None)
 (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
 (out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n')
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
print(f"{report['status']} {len(report['checks'])} checks; actual stock client with mock transports only")
