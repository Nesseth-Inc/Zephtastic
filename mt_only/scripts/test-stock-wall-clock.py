#!/usr/bin/env python3
"""Actual Python 2.7.11 setTime API against native Companion + RAM WallClock.
No serial/BLE driver, physical radio, key generation or OS clock modification.
--stock verifies hash-pinned primary source statements, not stock execution.
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
import sys
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
from meshtastic.protobuf import admin_pb2
from meshtastic.node import Node
mesh,port=client.mesh_pb2,client.portnums_pb2
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
files=[f for directory in (app/'src',vendor,mono,proto) for f in directory.rglob('*') if f.is_file() and f.suffix in ('.h','.cpp','.c')]
files += [Path(__file__).resolve(),*(app/('tests/'+s) for s in ('wall_clock_fixture.h','device_config_fixture.h','wall_clock_oracle.cpp','stock-private-client/client.py','stock-private-client/provenance.py'))]
def hashes():return {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(files)}
before=hashes();report=dict(status='FAIL',hardware=False,RF=False,keys=False,OS_clock_modified=False,ASAN_UBSAN=True,short_enums=a.short_enums,
 **provenance.source_provenance(root),python=client.provenance(),node_api_sha256=hashlib.sha256(inspect.getsource(Node).encode()).hexdigest(),checks=[])
def check(name,value):
 report['checks'].append(dict(name=name,status='PASS' if value else 'FAIL'))
 if not value:raise AssertionError(name)
def fields(raw):
 """Read pinned wire presence separately from old Python scalar-presence API."""
 offset=0;result=[]
 def varint():
  nonlocal offset
  value=0
  for shift in range(0,70,7):
   assert offset<len(raw);b=raw[offset];offset+=1;value|=(b&127)<<shift
   if b<128:return value
  raise AssertionError('oversized varint')
 while offset<len(raw):
  key=varint();tag,kind=key>>3,key&7;assert tag
  if kind==0:value=varint()
  elif kind in (1,5):
   size=8 if kind==1 else 4;value=raw[offset:offset+size];assert len(value)==size;offset+=size
  elif kind==2:
   size=varint();value=raw[offset:offset+size];assert len(value)==size;offset+=size
  else:raise AssertionError('unsupported wire type')
  result.append((tag,kind,value))
 return result
commands=[];transcripts=[]
try:
 if a.stock:
  stock=a.stock.resolve()
  pinned={'src/modules/AdminModule.cpp':'ee758ed26686b718f592d7a9df048cefecfca13a2c7a5d01fd11192b0de73a28','src/gps/RTC.cpp':'89c35911041374e929a6f4653215cf90aebb870a390b16d98b7be22a5a854a09','src/mesh/Router.cpp':'d3b4178cdb1ef81a130d70bbe702e857f7ffddae592e551cc907f580ab165620'}
  check('pinned primary-source hashes',all(hashlib.sha256((stock/name).read_bytes()).hexdigest()==digest for name,digest in pinned.items()))
  admin=(stock/'src/modules/AdminModule.cpp').read_text();rtc=(stock/'src/gps/RTC.cpp').read_text();router=(stock/'src/mesh/Router.cpp').read_text()
  check('stock time-only handler uses nonforced phone/NTP quality','tv.tv_sec = r->set_time_only;' in admin and 'perhapsSetRTC(RTCQualityNTP, &tv, false);' in admin)
  check('stock thirty-minute correction throttle','q == RTCQualityNTP && !Throttle::isWithinTimespanMs(lastSetMsec, (30 * 60 * 1000UL))' in rtc)
  check('stock optional timestamp presence','p->has_rx_time = ts.valid;' in router)
  report['stock_reference']=dict(revision='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c',sha256=pinned,comparison='source statements and wire schema; stock runtime not executed')
 check('actual setTime fixed32 tag43',admin_pb2.AdminMessage(set_time_only=1700000000).SerializeToString()==b'\xdd\x02\x00\xf1\x53\x65')
 check('literal zero retains set_time_only oneof',admin_pb2.AdminMessage(set_time_only=0).SerializeToString()==b'\xdd\x02\0\0\0\0')
 report['client_rx_time_has_presence']=mesh.MeshPacket.DESCRIPTOR.fields_by_name['rx_time'].has_presence
 (out/'meshtastic').mkdir(exist_ok=True)
 for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
 flags=['-fsanitize=address,undefined','-g','-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
 cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
 host_link=[]
 if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
 if a.short_enums:flags+=['-fshort-enums']
 objects=[]
 for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
  obj=out/(src.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(src),'-o',str(obj)];commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(obj))
 executable=out/'wall-clock-oracle'
 cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(app/'tests/wall_clock_oracle.cpp'),str(app/'src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*host_link,'-o',str(executable)]
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
     check(label+' no radio, TX or storage activation',not row['radio_sends'] and not row['radio_starts'] and not row['tx_armed'] and not row['writes'])
     return row
    def snapshot(who,via):
     with patch.object(client.mesh_interface.random,'randint',return_value=71):who._startConfig()
     row=exchange(via+' '+who.sent[-1].hex());who.accept(row['records_hex']);return row
    def timestamps(row,want):
     for raw in row['records_hex']:
      record=mesh.FromRadio.FromString(bytes.fromhex(raw))
      if record.HasField('packet'):
       check(label+' decoded UTC value',record.packet.rx_time==want)
       packet_wire=next(value for tag,kind,value in fields(bytes.fromhex(raw)) if tag==mesh.FromRadio.DESCRIPTOR.fields_by_name['packet'].number and kind==2)
       stamp=[(kind,value) for tag,kind,value in fields(packet_wire) if tag==7]
       check(label+' pinned optional timestamp wire presence',stamp==([(5,want.to_bytes(4,'little'))] if want else []))
    def request(who,via,action,expected=0):
     count=len(who.sent);action();pending=who.sent[count:];check(label+' bounded actual API request count',len(pending) in (1,2))
     if len(pending)==2:
      first=admin_pb2.AdminMessage.FromString(mesh.ToRadio.FromString(pending[0]).packet.decoded.payload)
      check(label+' real session-config preflight',first.HasField('get_config_request') and first.get_config_request==admin_pb2.AdminMessage.SESSIONKEY_CONFIG)
     for raw in pending:
      packet=mesh.ToRadio.FromString(raw).packet
      check(label+' stock local admin PKI hint normalized',packet.to==0x12345678 and packet.pki_encrypted and packet.decoded.portnum==port.ADMIN_APP)
      row=exchange(via+' '+raw.hex());records=[mesh.FromRadio.FromString(bytes.fromhex(r)) for r in row['records_hex']]
      check(label+' correlated queue first',bool(records) and records[0].HasField('queueStatus') and records[0].queueStatus.mesh_packet_id==packet.id)
      packets=[r.packet for r in records if r.HasField('packet')]
      check(label+' one fresh correlated reply',len(packets)==1 and packets[0].decoded.request_id==packet.id and not packets[0].pki_encrypted and not packets[0].public_key)
      callbacks=[];who._addResponseHandler(packet.id,callbacks.append,ackPermitted=True);who.accept(row['records_hex']);check(label+' stock callback',len(callbacks)==1)
      reply=packets[0]
      if reply.decoded.portnum==port.ROUTING_APP:
       check(label+' explicit expected result',mesh.Routing.FromString(reply.decoded.payload).error_reason==expected)
      else:check(label+' getter or session reply',reply.decoded.portnum==port.ADMIN_APP)
      timestamps(row,row['utc'])
     return row
    def echo(want):
     p=mesh.MeshPacket(rx_time=12345);p.decoded.portnum=port.TEXT_MESSAGE_APP;p.decoded.payload=b'fresh local timestamp'
     actual._sendPacket(p,destinationId=0x12345678,wantAck=True)
     row=exchange(transport+' '+actual.sent[-1].hex());records=[mesh.FromRadio.FromString(bytes.fromhex(r)) for r in row['records_hex']]
     check(label+' one self echo and local receipt',len([r for r in records if r.HasField('packet')])==2)
     timestamps(row,want);actual.accept(row['records_hex']);return row
    try:
     row=snapshot(actual,transport);snapshot(peer,opposite);check(label+' fresh boot unknown',row['utc']==0);unknown=echo(0)
     row=request(actual,transport,lambda:actual.localNode.setTime(1700000000));check(label+' explicit time applied',row['utc']==1700000000)
     exchange('monotonic 999');request(peer,opposite,lambda:peer.localNode.setTime(1700000000));exchange('monotonic 1000');echo(1700000001)
     request(actual,transport,lambda:actual.localNode.setTime(1700000000),mesh.Routing.RATE_LIMIT_EXCEEDED)
     request(actual,transport,lambda:actual.localNode._sendAdmin(admin_pb2.AdminMessage(set_time_only=0)),mesh.Routing.BAD_REQUEST)
     # Time-only malformed fixed32 must never reach the provider.
     for raw_admin in (b'\xdd\x02\0',b'\xd8\x02\x01'):
      request(actual,transport,lambda raw=raw_admin:actual.sendData(raw,destinationId=0x12345678,portNum=port.ADMIN_APP,wantAck=True,pkiEncrypted=True),mesh.Routing.BAD_REQUEST)
     exchange('monotonic 1799999');request(actual,transport,lambda:actual.localNode.setTime(100),mesh.Routing.RATE_LIMIT_EXCEEDED)
     exchange('monotonic 1800000');row=request(peer,opposite,lambda:peer.localNode.setTime(100));check(label+' backward UTC correction accepted at guard',row['utc']==100)
     exchange('monotonic 1801000');request(actual,transport,lambda:actual.localNode.requestConfig(admin_pb2.AdminMessage.DEVICE_CONFIG));echo(101)
     # Old bytes stay unknown; accepting a clock does not backfill queued/history.
     timestamps(unknown,0)
     exchange('monotonic 3600000')
     with patch('meshtastic.node.time.time',return_value=1700000123):row=request(actual,transport,lambda:actual.localNode.setTime())
     check(label+' default actual API uses host time value',row['utc']==1700000123)
     row=exchange('restart');check(label+' restart is unset',row['utc']==0);snapshot(actual,transport)
     request(actual,transport,lambda:actual.localNode.setTime(0xffffffff));exchange('monotonic 1000');echo(0)
     request(actual,transport,lambda:actual.localNode.setTime(1),mesh.Routing.RATE_LIMIT_EXCEEDED)
     exchange('monotonic 1800000');request(actual,transport,lambda:actual.localNode.setTime(1));echo(1)
     exchange('monotonic 1799999');request(actual,transport,lambda:actual.localNode.setTime(1),mesh.Routing.BAD_REQUEST);echo(0)
    finally:
     process.stdin.close()
     try:process.wait(timeout=5)
     except subprocess.TimeoutExpired:process.kill();process.wait();raise
    check(label+' oracle exited successfully',process.returncode==0)
 check('source and fixtures stable',hashes()==before);report['source_sha256']=before;report['status']='PASS'
finally:
 (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
 (out/'transcripts.json').write_text(json.dumps(transcripts,indent=2)+'\n')
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
print(f"{report['status']} {len(report['checks'])} checks; actual stock client, memory-only transports and clock")
