#!/usr/bin/env python3
"""Independent producer/real stock first-contact verification; no transport."""
import argparse,hashlib,importlib.util,json,os,shutil,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=a.source.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);(out/'RESULTS.json').unlink(missing_ok=True)
sys.path.insert(0,str(root));import host_test_toolchain as host
here=Path(__file__).resolve().parent;app=root/'zephcore';source=app/'bench/upstream-differential/source';crypto=source/'crypto-reference/Crypto';mono=app/'lib/monocypher';vendor=app/'adapters/bench/vendor'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
reference=module('reference',app/'bench/run-mt-signature-tests.py');extract=module('extract',app/'bench/run-routing-differential.py')
firmware=reference.verify_lock(source/'source-lock.json',source);cryptolock=reference.verify_lock(source/'crypto-reference/crypto-source-lock.json',source/'crypto-reference')
crcroot=source/'crypto-reference/ErriezCRC32';reference.verify_lock(crcroot/'source-lock.json',crcroot)
generated=reference.oracle(out,firmware)
with generated.open('a') as f:f.write('''
extern "C" void mtPinnedSignWithScalar(const uint8_t *scalar,const uint8_t *hedge,const uint8_t *message,size_t n,uint8_t *signature){
 uint8_t copied[32],secret[32],ed[32];memcpy(copied,scalar,32);XEdDSA::priv_curve_to_ed_keys(copied,secret,ed);
 memset(signature,0,64);memcpy(signature,hedge,32);XEdDSA::sign(signature,secret,ed,message,n);clean(copied);clean(secret);clean(ed);
}
''')
body,first,last=extract.extract((source/'Router.cpp').read_text(),'static NodeInfoBootstrapResult verifyFirstContactNodeInfo(')
bootstrap=out/'first-contact-oracle.cpp'
bootstrap.write_text(r'''#include "meshtastic/mesh.pb.h"
#include "MtCodec.h"
#include "ErriezCRC32.h"
#include <pb_decode.h>
#include <cstring>
extern "C" size_t mtPinnedSigningBuffer(uint32_t,uint32_t,uint32_t,const bench::RouteMetadata *,uint8_t *,size_t);
extern "C" bool mtPinnedSignatureVerify(const uint8_t *,const uint8_t *,const uint8_t *,size_t);
struct meshtastic_NodeInfoLite{struct {size_t size=0;uint8_t bytes[32]{};}public_key;bool verified=false;};
struct Database{meshtastic_NodeInfoLite node;bool allow=true;meshtastic_NodeInfoLite *getOrCreateMeshNode(uint32_t){return allow?&node:nullptr;}};
static Database db;static Database *nodeDB=&db;
constexpr unsigned NODEINFO_BITFIELD_HAS_XEDDSA_SIGNED_MASK=1;
static void nodeInfoLiteSetBit(meshtastic_NodeInfoLite *node,unsigned,bool v){node->verified=v;}
static bool pb_decode_from_bytes(const uint8_t *p,size_t n,const pb_msgdesc_t *fields,void *out){auto s=pb_istream_from_buffer(p,n);return pb_decode(&s,fields,out);}
struct ActualSignature{bool xeddsa_verify(const uint8_t *key,uint32_t from,uint32_t id,uint32_t to,const meshtastic_Data *d,const uint8_t *sig){
 bench::RouteMetadata m;m.portnum=d->portnum;m.request_id=d->request_id;m.reply_id=d->reply_id;m.emoji=d->emoji;
 m.wantResponse=d->want_response;m.hasBitfield=d->has_bitfield;m.bitfield=d->bitfield;m.payloadLength=d->payload.size;memcpy(m.payload,d->payload.bytes,d->payload.size);
 uint8_t bytes[267];const auto n=mtPinnedSigningBuffer(from,id,to,&m,bytes,sizeof(bytes));return mtPinnedSignatureVerify(key,sig,bytes,n);
}};static ActualSignature actual;static ActualSignature *crypto=&actual;
#define LOG_DEBUG(...) ((void)0)
enum class NodeInfoBootstrapResult { NOT_APPLICABLE, VERIFIED, INVALID };
'''+body+r'''
extern "C" int reviewedStockBootstrap(const uint8_t *wire,size_t n,uint32_t from,uint32_t id,uint32_t to,bool allow,uint8_t *key){
 meshtastic_MeshPacket p{};p.from=from;p.to=to;p.id=id;
 if(!pb_decode_from_bytes(wire,n,meshtastic_Data_fields,&p.decoded)||p.decoded.xeddsa_signature.size!=64)return 2;
 db={};db.allow=allow;const auto result=verifyFirstContactNodeInfo(&p);
 if(result==NodeInfoBootstrapResult::VERIFIED){if(!p.xeddsa_signed||!db.node.verified||db.node.public_key.size!=32)return 2;memcpy(key,db.node.public_key.bytes,32);}
 return int(result);
}
''')
mesh=out/'includes/meshtastic';mesh.mkdir(parents=True,exist_ok=True)
for h in source.glob('*.pb.h'):shutil.copyfile(h,mesh/h.name)
flags=['-g','-O1','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer']
flags+=['-I'+str(x) for x in (root/'mt_only/src',vendor,mono,crypto,source,source/'nanopb',out/'includes',crcroot/'src')]
inputs=[root/'mt_only/src/pki'/x for x in ('PkiSigner.h','PkiSigner.cpp','PkiIdentity.h','PkiIdentity.cpp')]+[vendor/'MtSignature.h',vendor/'MtSignature.cpp',vendor/'MtCodec.h']
before={str(x.relative_to(root)):sha(x) for x in inputs};commands=[];objects=[]
def run(cmd):commands.append(cmd);subprocess.run(cmd,check=True)
for src in [mono/'monocypher.c',mono/'monocypher-ed25519.c',crcroot/'src/ErriezCRC32.c',*[source/'nanopb'/x for x in ('pb_common.c','pb_encode.c','pb_decode.c')]]:
    obj=out/(src.name+'.o');run([*host.cc(),'-std=c11',*flags,'-c',str(src),'-o',str(obj)]);objects.append(str(obj))
for name in ('XEdDSA.cpp','Ed25519.cpp','Curve25519.cpp','SHA512.cpp','Hash.cpp','Crypto.cpp','BigNumberUtil.cpp'):
    obj=out/(name+'.o');run([*host.cxx(),'-std=c++17',*flags,*(['-fno-sanitize=shift-base'] if name=='XEdDSA.cpp' else []),'-c',str(crypto/name),'-o',str(obj)]);objects.append(str(obj))
test=here/'m10_signer_adversarial.cpp'
run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,str(test),str(root/'mt_only/src/pki/PkiSigner.cpp'),str(root/'mt_only/src/pki/PkiIdentity.cpp'),str(root/'mt_only/src/Protobuf.cpp'),str(vendor/'MtSignature.cpp'),str(generated),str(bootstrap),*objects,host.dead_strip_flag(),'-o',str(out/'signer-tests')])
r=subprocess.run([str(out/'signer-tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
stable=before=={str(x.relative_to(root)):sha(x) for x in inputs}
try:head=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True,stderr=subprocess.DEVNULL).strip() if (root/'.git').exists() else None
except (OSError,subprocess.CalledProcessError):head=None
receipt={'status':'PASS' if r.returncode==0 and stable else 'FAIL','source_commit':head,'source_stable':stable,'source_sha256':before,'ASAN_UBSAN':True,'reference_exception':'Unchanged stock XEdDSA.cpp shift-base only; candidate fully instrumented','stock_commit':firmware['commit'],'crypto_commit':cryptolock['commit'],'groups':r.stdout.splitlines(),'exit_code':r.returncode,'first_contact_oracle':{'function':'verifyFirstContactNodeInfo','first_line':first,'last_line':last,'sha256':hashlib.sha256(body.encode()).hexdigest()},'test_sha256':sha(test),'runner_sha256':sha(Path(__file__)),'fixture_boundaries':['Actual pinned protobuf decoder, CRC32, signing buffer, XEdDSA verification and whole stock first-contact decision function','Only NodeDB allocation and signer-bit side effects substituted','Public synthetic provider and entropy fixtures only','No devices, RF, persistent credentials, runtime qualification or human identity assurance']}
(out/'RESULTS.json').write_text(json.dumps(receipt,indent=2)+'\n');(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
raise SystemExit(r.returncode if stable else 2)
