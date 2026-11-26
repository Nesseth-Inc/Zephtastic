#!/usr/bin/env python3
"""Host-only stock Python private-message shapes/parser against native Companion.

No serial/BLE/network transport is imported or opened. Default firmware has no
credential provider; runtime crypto success belongs to the separately injected
M10 fixture. Synthetic readback tests validate client interpretation only.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shlex
from pathlib import Path
import shutil
import subprocess
import sys
from unittest.mock import patch

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2])
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--baseline-m9-self-echo',action='store_true',help='Record known unsafe M9 self echo; never a release acceptance mode')
a=parser.parse_args();root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
app=Path(__file__).resolve().parents[1];fixture=app/'tests/stock-private-client'
spec=importlib.util.spec_from_file_location('stock_private_client',fixture/'client.py')
client=importlib.util.module_from_spec(spec);spec.loader.exec_module(client)
spec=importlib.util.spec_from_file_location('source_provenance',fixture/'provenance.py')
source=importlib.util.module_from_spec(spec);spec.loader.exec_module(source)
mesh,port=client.mesh_pb2,client.portnums_pb2
local,peer=0x12345678,0x87654321
# Public-only parser/shape data; not a cryptographic identity or generated key.
peer_public=bytes(range(32))
cases=client.build_requests(local,peer,peer_public)
provenance=client.provenance()
(out/'requests.json').write_text(json.dumps(dict(provenance=provenance,cases=cases),indent=2)+'\n')
proto=root/'zephcore/bench/upstream-differential/source';vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher'
(out/'meshtastic').mkdir(exist_ok=True)
for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
flags=['-fsanitize=address,undefined','-g','-I'+str(root/'mt_only/src'),'-I'+str(vendor),'-I'+str(mono),'-I'+str(out),'-I'+str(proto/'nanopb')]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'));host_link=[]
if sys.platform.startswith('linux'):flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
commands=[];objects=[]
source_files=[p for folder in (root/'mt_only/src',vendor,mono,proto) for p in folder.rglob('*') if p.is_file() and p.suffix in ('.h','.cpp','.c')]
def state():return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(source_files)}
before=state()
fixture_paths=[Path(__file__).resolve(),*(fixture/n for n in ('client.py','oracle.cpp','admin-matrix.json','provenance.py'))]
fixture_hashes={str(p.relative_to(app)):hashlib.sha256(p.read_bytes()).hexdigest() for p in fixture_paths}
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
 obj=out/(src.stem+'.o');cmd=[*cc,'-std=c11',*flags,'-c',str(src),'-o',str(obj)];subprocess.run(cmd,check=True);commands.append(cmd);objects.append(str(obj))
cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(fixture/'oracle.cpp'),str(root/'mt_only/src/Protobuf.cpp'),str(vendor/'MtCodec.cpp'),str(vendor/'MtSignature.cpp'),*objects,*host_link,'-o',str(out/'stock-private-oracle')]
subprocess.run(cmd,check=True);commands.append(cmd)
report=dict(status='FAIL',hardware=False,RF=False,credential_provider=False,private_crypto_success_tested=False,
            **source.source_provenance(root),
            python=provenance,checks=[],known_gaps=[],fixture_sha256=fixture_hashes)
checks=report['checks'];all_records={}
def check(name,predicate):
 checks.append(dict(name=name,status='PASS' if predicate else 'FAIL'))
 if not predicate:raise AssertionError(name)
try:
 for transport in ('usb','ble'):
  # These names select actual Companion framing only, never physical transports.
  requests=[mesh.ToRadio(want_config_id=123).SerializeToString().hex()]+[c['toradio_hex'] for c in cases]
  result=subprocess.run([str(out/'stock-private-oracle'),transport],input='\n'.join(requests)+'\n',text=True,capture_output=True,timeout=10,
                        env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
  (out/(transport+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
  responses=[json.loads(line) for line in result.stdout.splitlines()];check(transport+' source response count',len(responses)==len(requests))
  check(transport+' no radio start/send',all(row['radio_sends']==0 and row['radio_starts']==0 and row['parse_errors']==0 for row in responses))
  handshake=[mesh.FromRadio.FromString(bytes.fromhex(raw)) for raw in responses[0]['records_hex']]
  metadata=next(r.metadata for r in handshake if r.HasField('metadata'))
  owner=next(r.node_info.user for r in handshake if r.HasField('node_info') and r.node_info.num==local)
  check(transport+' no-provider capability/key truthful',not metadata.hasPKC and not owner.public_key)
  module_records=[r.moduleConfig for r in handshake if r.HasField('moduleConfig')]
  module_names=[m.WhichOneof('payload_variant') for m in module_records]
  known_module_names={f.name for f in module_records[0].DESCRIPTOR.oneofs_by_name['payload_variant'].fields}
  check(transport+' disabled module getter handshake matches installed client schema',
        len(module_records)==17 and set(module_names)-{None}==known_module_names and module_names.count(None)==1 and
        all(not client.wire(getattr(m,name)) for m,name in zip(module_records,module_names) if name))
  for case,response in zip(cases,responses[1:]):
   name=transport+': '+case['name'];records=[mesh.FromRadio.FromString(bytes.fromhex(v)) for v in response['records_hex']]
   check(name+' queue is first and correlated',records and records[0].HasField('queueStatus') and records[0].queueStatus.mesh_packet_id==case['request_id'])
   observer=client.StockClient(local,peer,peer_public);callbacks=[]
   def got_response(packet):callbacks.append(packet)
   observer._addResponseHandler(case['request_id'],got_response,ackPermitted=True)
   observer.accept(response['records_hex'])
   routes=[r.packet for r in records if r.HasField('packet') and r.packet.decoded.portnum==port.ROUTING_APP]
   check(name+' exactly one correlated route/callback',len(routes)==1 and routes[0].decoded.request_id==case['request_id'] and len(callbacks)==1)
   route=mesh.Routing.FromString(routes[0].decoded.payload)
   if case['name'].startswith('self_'):
    echoes=[r.packet for r in records if r.HasField('packet') and r.packet.decoded.portnum==port.TEXT_MESSAGE_APP]
    safe=not echoes or all(not p.pki_encrypted and not p.public_key and not client.raw_varint_field(client.wire(p),23) for p in echoes)
    if a.baseline_m9_self_echo:
     check(name+' baseline gap reproduced',not safe)
     report['known_gaps'].append(case['name']+': M9 local echo preserves unverified client authentication metadata')
    else:check(name+' no caller authentication claims echoed',safe)
   else:
    reason=mesh.Routing.NO_INTERFACE if case['name'].startswith('ordinary_') else mesh.Routing.NO_CHANNEL if case['name']=='private_secondary_channel' else mesh.Routing.PKI_FAILED
    allowed={reason} if reason in (mesh.Routing.NO_INTERFACE,mesh.Routing.NO_CHANNEL) else {mesh.Routing.PKI_FAILED,mesh.Routing.NO_INTERFACE}
    if case['name']=='private_application_response':allowed.add(mesh.Routing.BAD_REQUEST)
    check(name+' truthful blocked default-provider error',route.error_reason in allowed)
    response['routing_error']=mesh.Routing.Error.Name(route.error_reason)
    check(name+' no private plaintext echo',not any(r.HasField('packet') and r.packet.decoded.portnum==port.TEXT_MESSAGE_APP for r in records))
   all_records[name]=response
 # Real Node API marks even local ADMIN packets PKI. Local administration is
 # authorized by the local transport; hints must not be treated as proof or
 # echoed into newly constructed responses.
 from meshtastic.protobuf import admin_pb2
 actual=client.StockClient(local,peer,peer_public)
 actual.localNode._sendAdmin(admin_pb2.AdminMessage(get_owner_request=True))
 normal=mesh.ToRadio.FromString(actual.sent[-1])
 check('real local Node._sendAdmin uses PKI hint',normal.packet.to==local and normal.packet.pki_encrypted)
 claimed=mesh.ToRadio();claimed.CopyFrom(normal);claimed.packet.public_key=peer_public
 claimed.packet.decoded.ParseFromString(client.wire(claimed.packet.decoded)+b'\x52\x40'+b'x'*64)
 claimed.packet.ParseFromString(client.wire(claimed.packet)+b'\xb8\x01\x01')
 for transport in ('usb','ble'):
  result=subprocess.run([str(out/'stock-private-oracle'),transport],input='\n'.join(client.wire(v).hex() for v in (normal,claimed))+'\n',text=True,capture_output=True,timeout=10,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
  result.check_returncode();responses=[json.loads(line) for line in result.stdout.splitlines()]
  check(transport+' local admin response count',len(responses)==2)
  for label,response in zip(('stock','untrusted_hints'),responses):
   readback=[mesh.FromRadio.FromString(bytes.fromhex(v)) for v in response['records_hex']]
   packets=[r.packet for r in readback if r.HasField('packet')]
   check(transport+' '+label+' local admin fresh unauthenticated reply',
         len(packets)==1 and packets[0].decoded.portnum==port.ADMIN_APP and
         packets[0].decoded.request_id==normal.packet.id and
         admin_pb2.AdminMessage.FromString(packets[0].decoded.payload).HasField('get_owner_response') and
         not packets[0].pki_encrypted and not packets[0].public_key and b'\x52\x40'+b'x'*64 not in client.wire(packets[0].decoded) and
         not client.raw_varint_field(client.wire(packets[0]),23) and not response['radio_sends'])
   all_records[transport+': local_admin_'+label]=response
 # Real Python 2.7.11 settings-screen writes for unchanged disabled modules.
 # Its supported Node.writeConfig names produce one canonical empty selected
 # submessage. Newer pinned-only status/TAK/MeshBeacon variants are exercised by
 # the native metadata matrix rather than invented as client support here.
 module_fields={
  'mqtt':1,'serial':2,'external_notification':3,'store_forward':4,
  'range_test':5,'telemetry':6,'canned_message':7,'audio':8,
  'remote_hardware':9,'neighbor_info':10,'ambient_lighting':11,
  'detection_sensor':12,'paxcounter':13,'traffic_management':15,
 }
 module_client=client.StockClient(local,peer,peer_public);module_vectors=[]
 for name,tag in module_fields.items():
  module_client.localNode.writeConfig(name);request=mesh.ToRadio.FromString(module_client.sent[-1])
  payload=bytes(request.packet.decoded.payload);inner=bytes([(tag<<3)|2,0])
  expected=b'\x9a\x02'+bytes([len(inner)])+inner
  parsed=admin_pb2.AdminMessage.FromString(payload)
  check('real client canonical disabled module wire '+name,
        request.packet.to==local and request.packet.pki_encrypted and parsed.WhichOneof('payload_variant')=='set_module_config' and
        parsed.set_module_config.WhichOneof('payload_variant')==name and payload==expected)
  module_vectors.append((name,request))
 for transport in ('usb','ble'):
  result=subprocess.run([str(out/'stock-private-oracle'),transport],input='\n'.join(client.wire(v).hex() for _,v in module_vectors)+'\n',
                       text=True,capture_output=True,timeout=10,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
  (out/('module-noop-'+transport+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
  responses=[json.loads(line) for line in result.stdout.splitlines()]
  check(transport+' real client disabled module no-op count',len(responses)==len(module_vectors))
  for (name,request),response in zip(module_vectors,responses):
   records=[mesh.FromRadio.FromString(bytes.fromhex(raw)) for raw in response['records_hex']]
   routes=[r.packet for r in records if r.HasField('packet') and r.packet.decoded.portnum==port.ROUTING_APP]
   check(transport+' real client disabled module no-op '+name,
         len(records)==2 and records[0].HasField('queueStatus') and records[0].queueStatus.mesh_packet_id==request.packet.id and
         len(routes)==1 and routes[0].decoded.request_id==request.packet.id and
         mesh.Routing.FromString(routes[0].decoded.payload).error_reason==mesh.Routing.NONE and
         response['settings_saves']==0 and response['radio_sends']==0 and response['radio_starts']==0 and response['parse_errors']==0)
   all_records[transport+': module_noop_'+name]=response
 report['module_noop_client_matrix']=dict(client_version=provenance['version'],write_config_variants=len(module_vectors),
                                          transports=['usb','ble'],physical_transport=False,modules_remain_excluded=True)
 # Exhaustive pinned admin inventory: distinguish supported controls from
 # response-only fields and explicitly rejected requests using actual decoder.
 matrix=json.loads((fixture/'admin-matrix.json').read_text())
 header=(proto/'admin.pb.h').read_text()
 schema=re.findall(r'X\(a, STATIC,\s+ONEOF,\s+(\w+),\s+\(payload_variant,(\w+),\w+\),\s+(\d+)\)',header)
 check('matrix covers all pinned admin oneof fields',
       {(name,int(tag),kind) for kind,name,tag in schema}=={(r['name'],r['tag'],r['wire_kind']) for r in matrix['requests_and_responses']})
 check('pinned admin schema unchanged',hashlib.sha256(header.encode()).hexdigest()==matrix['admin_schema_sha256'])
 companion=(root/'mt_only/src/Companion.h').read_text()
 admin_body=companion.split('void handleAdmin(',1)[1].split('void notification(',1)[0]
 branches=set(re.findall(r'case meshtastic_AdminMessage_(\w+)_tag:',admin_body))
 check('documented supported controls match dispatch',branches=={r['name'] for r in matrix['requests_and_responses'] if r['status'] not in ('deferred','response-only')})
 def vi(value):
  data=bytearray()
  while value>127:data.append((value&127)|128);value>>=7
  data.append(value);return bytes(data)
 unsupported=[r for r in matrix['requests_and_responses'] if r['status'] in ('deferred','response-only')]
 admin_requests=[]
 for i,row in enumerate(unsupported):
  tag,kind=row['tag'],row['wire_kind']
  if kind in ('MESSAGE','STRING'):payload=vi(tag*8+2)+b'\0'
  elif kind=='FIXED32':payload=vi(tag*8+5)+b'\0'*4
  else:payload=vi(tag*8)+vi(1)
  r=mesh.ToRadio();r.packet.id=0x4d300001+i;r.packet.to=local;r.packet.want_ack=True;r.packet.decoded.portnum=port.ADMIN_APP;r.packet.decoded.want_response=True;r.packet.decoded.payload=payload
  admin_requests.append(client.wire(r).hex())
 for transport in ('usb','ble'):
  result=subprocess.run([str(out/'stock-private-oracle'),transport],input='\n'.join(admin_requests)+'\n',text=True,capture_output=True,timeout=10,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
  (out/('admin-'+transport+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
  responses=[json.loads(line) for line in result.stdout.splitlines()]
  check(transport+' unsupported admin count',len(responses)==len(unsupported))
  for i,(row,response) in enumerate(zip(unsupported,responses)):
   records=[mesh.FromRadio.FromString(bytes.fromhex(raw)) for raw in response['records_hex']]
   check(transport+': reject unsupported '+row['name'],
         len(records)==3 and records[0].HasField('queueStatus') and records[1].HasField('clientNotification') and
         records[1].clientNotification.reply_id==0x4d300001+i and records[2].HasField('packet') and
         records[2].packet.decoded.request_id==0x4d300001+i and
         mesh.Routing.FromString(records[2].packet.decoded.payload).error_reason==mesh.Routing.BAD_REQUEST and
         response['radio_sends']==0 and response['radio_starts']==0)
 report['admin_inventory']=dict(oneof_fields=len(schema),supported_controls=len(branches),deferred_requests=sum(r['status']=='deferred' for r in matrix['requests_and_responses']),response_only=sum(r['status']=='response-only' for r in matrix['requests_and_responses']))
 # Real client conversion of successful-looking private receive metadata. This
 # tests display/parser contracts only and deliberately does not claim crypto.
 r=mesh.FromRadio();p=r.packet;setattr(p,'from',peer);p.to=local;p.id=0x4d200001;p.pki_encrypted=True;p.public_key=peer_public;p.channel=0
 p.decoded.portnum=port.TEXT_MESSAGE_APP;p.decoded.payload='M10 private receive ✓'.encode();p.decoded.reply_id=0x4d001122;p.decoded.emoji=1
 observer,events=client.inspect_records([client.wire(r)],local,peer,peer_public)
 check('stock parser preserves private sender/key/reply/emoji',len(events)==1 and events[0]['from']==peer and events[0]['to']==local and events[0]['pkiEncrypted'] is True and
       events[0]['publicKey']==client.base64.b64encode(peer_public).decode() and events[0]['decoded']['replyId']==0x4d001122 and events[0]['decoded']['emoji']==1 and events[0]['decoded']['text']=='M10 private receive ✓')
 # Tag23 is absent from Python2.7.11 but raw protobufs must retain it.
 q=mesh.MeshPacket();setattr(q,'from',peer);q.to=local;q.id=0x4d200002;q.decoded.portnum=port.ROUTING_APP;q.decoded.request_id=cases[0]['request_id'];q.decoded.payload=client.wire(mesh.Routing(error_reason=mesh.Routing.NONE))
 q.ParseFromString(client.wire(q)+b'\xb8\x01\x01')
 r=mesh.FromRadio();r.packet.CopyFrom(q);observer,events=client.inspect_records([client.wire(r)],local,peer,peer_public)
 check('client old schema cannot display proof enum but raw retains tag23',not provenance['knows_ack_proof_status'] and 'ackProofStatus' not in events[0] and client.raw_varint_field(client.wire(events[0]['raw']),23)==[1])
 # A public ACK can still invoke the client's ordinary callback. Runtime receipt
 # filtering must happen before forwarding it; client callback alone proves none.
 observer=client.StockClient(local,peer,peer_public);callbacks=[]
 observer._addResponseHandler(cases[0]['request_id'],lambda p:callbacks.append(p),ackPermitted=True)
 q=mesh.MeshPacket();setattr(q,'from',peer);q.to=local;q.decoded.portnum=port.ROUTING_APP;q.decoded.request_id=cases[0]['request_id'];q.decoded.payload=client.wire(mesh.Routing(error_reason=mesh.Routing.NONE));r=mesh.FromRadio();r.packet.CopyFrom(q);observer.accept([client.wire(r)])
 check('plain ACK callback is not authentication evidence',len(callbacks)==1 and not callbacks[0].get('pkiEncrypted',False))
 oversize=client.StockClient(local,peer,peer_public)
 try:oversize.sendData(b'x'*(mesh.Constants.DATA_PAYLOAD_LEN+1),destinationId=peer,portNum=port.TEXT_MESSAGE_APP,pkiEncrypted=True)
 except client.MeshInterface.MeshInterfaceError:check('stock oversize rejected before serialization',not oversize.sent)
 else:raise AssertionError('oversize accepted')
 report['status']='BASELINE_GAPS_REPRODUCED' if report['known_gaps'] else 'PASS'
except Exception as error:
 report['error']=type(error).__name__+': '+str(error)
finally:
 report['source_stable_during_run']=before==state()
 report['fixture_stable_during_run']=fixture_hashes=={str(p.relative_to(app)):hashlib.sha256(p.read_bytes()).hexdigest() for p in fixture_paths}
 if not report['source_stable_during_run'] or not report['fixture_stable_during_run']:report['status']='FAIL'
 (out/'source-sha256.json').write_text(json.dumps(before,indent=2)+'\n')
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 (out/'records.json').write_text(json.dumps(all_records,indent=2)+'\n')
 (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report,indent=2))
raise SystemExit(0 if report['status'] in ('PASS','BASELINE_GAPS_REPRODUCED') else 1)
