#!/usr/bin/env python3
"""Actual stock Python API/parser with an injected PSA private runtime; host only.

Synthetic deterministic TEST keys and memory journals only. No device, physical
radio, Bluetooth, serial, network or credential provisioning is used.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import platform
import shlex
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--output',required=True,type=Path)
p.add_argument('--psa-root',required=True,type=Path)
p.add_argument('--psa-library',required=True,type=Path)
a=p.parse_args();root=a.source.resolve();app=root/'mt_only';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
fixture=Path(__file__).resolve().parents[1]/'tests/stock-private-client'
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
client=module('stock_private_client',fixture/'client.py');source=module('source_provenance',fixture/'provenance.py')
mesh,port=client.mesh_pb2,client.portnums_pb2
vendor=root/'zephcore/adapters/bench/vendor';mono=root/'zephcore/lib/monocypher';proto=root/'zephcore/bench/upstream-differential/source';psa=a.psa_root.resolve();library=a.psa_library.resolve()
pin=next(x['commit'] for x in json.loads((app/'dependency-lock.json').read_text()) if x['path']=='modules/crypto/tf-psa-crypto')
assert (psa/'.git').exists() and subprocess.check_output(['git','-C',str(psa),'rev-parse','HEAD'],text=True).strip()==pin
subprocess.run(['git','-C',str(psa),'diff','--exit-code','HEAD','--'],check=True,stdout=subprocess.DEVNULL)
source_files=[p for folder in (app/'src',app/'experimental/pki',vendor,mono,proto) for p in folder.rglob('*') if p.is_file() and p.suffix in ('.h','.cpp','.c')]
source_files.extend((app/'tests/pki_runtime_fixture.h',app/'tests/pki/psa-host-config.h',app/'dependency-lock.json'))
def state():return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(source_files)}
before=state()
fixture_files=[Path(__file__).resolve(),*(fixture/n for n in ('client.py','runtime_oracle.cpp','provenance.py'))]
def fixture_state():return {str(p.relative_to(fixture.parents[1])):hashlib.sha256(p.read_bytes()).hexdigest() for p in fixture_files}
fixture_before=fixture_state();library_hash=hashlib.sha256(library.read_bytes()).hexdigest()
report=dict(status='FAIL',hardware=False,physical_RF=False,credential_provider='injected memory-only deterministic TEST fixture',
            private_crypto_success_tested=True,**source.source_provenance(root),python=client.provenance(),checks=[],
            psa_revision=pin,psa_library_sha256=library_hash,ASAN_UBSAN=True,fixture_sha256=fixture_before)
checks=report['checks'];commands=[];all_records={}
def check(name,ok):
 checks.append(dict(name=name,status='PASS' if ok else 'FAIL'))
 if not ok:raise AssertionError(name)
def run(cmd,**kwargs):
 commands.append([str(x) for x in cmd])
 try:return subprocess.run(cmd,check=True,**kwargs)
 except subprocess.CalledProcessError as error:
  (out/'failed-process.log').write_text((error.stdout or '')+(error.stderr or ''));raise
def records(raw):return [mesh.FromRadio.FromString(bytes.fromhex(x)) for x in raw]
def routes(raw,request):return [r.packet for r in records(raw) if r.HasField('packet') and r.packet.decoded.portnum==port.ROUTING_APP and r.packet.decoded.request_id==request]
def texts(raw):return [r.packet for r in records(raw) if r.HasField('packet') and r.packet.decoded.portnum==port.TEXT_MESSAGE_APP]
env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'}
try:
 (out/'meshtastic').mkdir(exist_ok=True)
 for h in proto.glob('*.pb.h'):shutil.copyfile(h,out/'meshtastic'/h.name)
 flags=['-fsanitize=address,undefined','-g','-O1',*[f'-I{x}' for x in (app/'src',app/'src/pki',app/'experimental/pki',app/'tests',vendor,mono,out,proto/'nanopb',psa/'include',psa/'drivers/builtin/include',psa/'dispatch/include')],f'-DTF_PSA_CRYPTO_CONFIG_FILE="{app / "tests/pki/psa-host-config.h"}"']
 cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'));host_link=[]
 if platform.system()=='Linux':flags+=['-DMT_HOST_OPENSSL=1'];host_link+=['-lcrypto']
 objects=[]
 for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',*(proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c'))]:
  obj=out/(src.stem+'.o');run([*cc,'-std=c11',*flags,'-c',src,'-o',obj]);objects.append(obj)
 exe=out/'stock-runtime-oracle'
 run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,fixture/'runtime_oracle.cpp',app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*[app/f'src/pki/{n}.cpp' for n in ('PkiRuntime','PkiSession','PkiIdentity','PkiLedger','PsaPkiBackend','PkiSigner')],app/'experimental/pki/PkiCrypto.cpp',app/'experimental/pki/PkiAck.cpp',*objects,library,*host_link,'-o',exe])
 info=json.loads(run([exe,'info'],capture_output=True,text=True,env=env,timeout=10).stdout);report['fixture_identity']=info
 local,peer=info['local'],info['peer'];key=bytes.fromhex(info['peer_public'])
 cases=client.build_requests(local,peer,key)
 check('actual stock API fixtures reproducible',cases==client.build_requests(local,peer,key))
 requests=[dict(c,mode='normal') for c in cases]
 for mode in ('crypto_failure','entropy_failure','unarmed','authenticated_nak'):
  requests.append(dict(cases[0],name=mode,mode=mode))
 unknown=mesh.ToRadio.FromString(bytes.fromhex(cases[0]['toradio_hex']));unknown.packet.to=0x12345678
 assert unknown.packet.to not in (local,peer)
 requests.append(dict(cases[0],name='untrusted_peer',mode='normal',toradio_hex=client.wire(unknown).hex()))
 (out/'requests.json').write_text(json.dumps(dict(python=client.provenance(),info=info,cases=requests),indent=2)+'\n')
 for transport in ('usb','ble'):
  r=run([exe,transport],input='\n'.join(c['mode']+' '+c['toradio_hex'] for c in requests)+'\n',capture_output=True,text=True,env=env,timeout=30)
  (out/(transport+'.log')).write_text(r.stdout+r.stderr);responses=[json.loads(line) for line in r.stdout.splitlines()]
  check(transport+' case count',len(responses)==len(requests));other='ble' if transport=='usb' else 'usb'
  for case,response in zip(requests,responses):
   name=transport+': '+case['name'];all_records[name]=response;request=mesh.ToRadio.FromString(bytes.fromhex(case['toradio_hex']));id=case['request_id']
   check(name+' exact request and framing',response['id']==id and response['parse_errors']==0)
   handshake=records(response['handshake']['usb']);metadata=next(r.metadata for r in handshake if r.HasField('metadata'));owner=next(r.node_info.user for r in handshake if r.HasField('node_info') and r.node_info.num==local)
   check(name+' provider capability and CRC identity advertised',metadata.hasPKC and owner.public_key.hex()==info['local_public'])
   submitted=response['submitted'][transport];first=records(submitted)[0]
   check(name+' first queue correlated',first.HasField('queueStatus') and first.queueStatus.mesh_packet_id==id)
   observer=client.StockClient(local,peer,key);callbacks=[];observer._addResponseHandler(id,lambda packet:callbacks.append(packet),ackPermitted=True);observer.accept(submitted)
   success=case['name'] in ('ordinary_direct_text','ordinary_direct_reply','explicit_private_key','explicit_private_no_client_key','authenticated_nak')
   if not success:
    expected=mesh.Routing.PKI_SEND_FAIL_PUBLIC_KEY if case['name']=='untrusted_peer' else mesh.Routing.NO_CHANNEL if case['name']=='private_secondary_channel' else mesh.Routing.BAD_REQUEST if case['name']=='private_application_response' else mesh.Routing.NO_INTERFACE if case['name']=='unarmed' else mesh.Routing.PKI_FAILED
    rp=routes(submitted,id)
    check(name+' exact error callback/no plaintext',len(rp)==1 and mesh.Routing.FromString(rp[0].decoded.payload).error_reason==expected and len(callbacks)==1 and not texts(submitted))
    check(name+' no Public fallback or other-client receipt',not response['frames'] and not response['submitted'][other] and not client.raw_varint_field(client.wire(rp[0]),23))
    check(name+' error notification correlated',any(r.HasField('clientNotification') and r.clientNotification.reply_id==id for r in records(submitted)))
    continue
   check(name+' queued without delivery claim',not callbacks and not routes(submitted,id) and not texts(submitted))
   actual=mesh.Data.FromString(bytes.fromhex(response['decrypted_data']));expected=mesh.Data();expected.CopyFrom(request.packet.decoded);expected.bitfield=0
   check(name+' exact recipient decrypted client Data',client.wire(actual)==client.wire(expected))
   frame=bytes.fromhex(response['frames'][0])
   check(name+' encrypted frame no exposed text',frame[13]==0 and int.from_bytes(frame[4:8],'little')==local and int.from_bytes(frame[:4],'little')==peer and request.packet.decoded.payload not in frame)
   for phase in ('completed','unproven_ack'):
    observer.accept(response[phase][transport]);check(name+' '+phase+' cannot complete client callback',not callbacks and not routes(response[phase][transport],id))
   proven=response['proven_ack'][transport];observer.accept(proven);rp=routes(proven,id);code=33 if case['mode']=='authenticated_nak' else 0
   check(name+' authenticated outcome invokes one correct callback',len(callbacks)==1 and len(rp)==1 and getattr(rp[0],'from')==peer and rp[0].to==local and mesh.Routing.FromString(rp[0].decoded.payload).error_reason==code)
   check(name+' raw proof status valid/old client schema honest',client.raw_varint_field(client.wire(rp[0]),23)==[1] and 'ackProofStatus' not in callbacks[0])
   check(name+' completion belongs only to requesting transport',not routes(response['proven_ack'][other],id))
   check(name+' authenticated result explained',any(r.HasField('clientNotification') and r.clientNotification.reply_id==id for r in records(proven)))
   for phase in ('tampered_receive','duplicate_receive','quota_duplicate_receive'):
    check(name+' '+phase+' no plaintext either transport',all(not texts(response[phase][t]) for t in ('usb','ble')))
   for t in ('usb','ble'):
    receiver,events=client.inspect_records(response['valid_receive'][t],local,peer,key);messages=[e for e in events if e.get('decoded',{}).get('portnum')=='TEXT_MESSAGE_APP']
    check(name+' authenticated receive '+t+' exact private metadata',len(messages)==1 and messages[0]['from']==peer and messages[0]['to']==local and messages[0]['pkiEncrypted'] and messages[0]['publicKey']==client.base64.b64encode(key).decode() and messages[0]['decoded']['replyId']==id and messages[0]['decoded']['emoji']==1 and messages[0]['decoded']['text']=='M10 authenticated reply')
   check(name+' initial and one bounded duplicate incoming proof ACK',len(response['frames'])==3 and response['incoming_ack_verified'] and response['duplicate_ack_verified'])
   check(name+' later duplicates exhaust ACK quota without redelivery',response['duplicate_ack_quota_verified'])
 report['status']='PASS'
except Exception as error:
 report['error']=type(error).__name__+': '+str(error)
finally:
 report['source_stable_during_run']=before==state();report['fixture_stable_during_run']=fixture_before==fixture_state();report['psa_library_stable_during_run']=library_hash==hashlib.sha256(library.read_bytes()).hexdigest()
 if not all(report[k] for k in ('source_stable_during_run','fixture_stable_during_run','psa_library_stable_during_run')):report['status']='FAIL'
 (out/'source-sha256.json').write_text(json.dumps(before,indent=2)+'\n');(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');(out/'records.json').write_text(json.dumps(all_records,indent=2)+'\n');(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(dict(status=report['status'],checks=len(checks),error=report.get('error'),source_stable=report['source_stable_during_run']),indent=2))
raise SystemExit(0 if report['status']=='PASS' else 1)
