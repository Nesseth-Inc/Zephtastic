#!/usr/bin/env python3
"""Offline stateless PKI differential; no firmware linkage, keys, devices or RF."""
import argparse,hashlib,importlib.util,json,os,platform,shlex,shutil,subprocess,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
SOURCE=ROOT/'zephcore/bench/upstream-differential/source'
CRYPTO=SOURCE/'crypto-reference/Crypto'
MONO=ROOT/'zephcore/lib/monocypher'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def lock(path,root):
    data=json.loads(path.read_text())
    for item in data['files']:
        p=root/item['local_file']
        if sha(p)!=item['sha256']:raise RuntimeError('Source lock changed: '+str(p))
    return data
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--openssl',type=Path,default=Path('/opt/homebrew/opt/openssl@3'))
p.add_argument('--psa-root',type=Path);p.add_argument('--psa-library',type=Path);p.add_argument('--psa-config',type=Path);a=p.parse_args()
if any([a.psa_root,a.psa_library,a.psa_config]) and not all([a.psa_root,a.psa_library,a.psa_config]):p.error('all PSA arguments are required together')
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').unlink(missing_ok=True)
firmware=lock(SOURCE/'source-lock.json',SOURCE)
lock(SOURCE/'crypto-reference/crypto-source-lock.json',SOURCE/'crypto-reference')
lock(HERE/'reference/source-lock.json',HERE/'reference')
spec=importlib.util.spec_from_file_location('extract',ROOT/'zephcore/bench/run-routing-differential.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
original=(SOURCE/'CryptoEngine.cpp').read_text();pieces=[];sections=[]
for marker in ['static void putLE32(', 'bool CryptoEngine::encryptCurve25519(', 'bool CryptoEngine::decryptCurve25519(', 'bool CryptoEngine::ackProofCompute(', 'void CryptoEngine::setDHPrivateKey(', 'void CryptoEngine::hash(', 'void CryptoEngine::aesSetKey(', 'void CryptoEngine::aesEncrypt(', 'bool CryptoEngine::setDHPublicKey(', 'void CryptoEngine::initNonce(']:
    body,first,last=extract.extract(original,marker);pieces.append(body)
    sections.append({'source':'CryptoEngine.cpp','function':marker,'first_line':first,'last_line':last,'sha256':hashlib.sha256(body.encode()).hexdigest()})
constants='\n'.join(line for line in original.splitlines() if line.startswith(('static const char ACK_PROOF_LABEL[]', '#define ACK_PROOF_LABEL_LEN')))
mesh=out/'includes/meshtastic';mesh.mkdir(parents=True,exist_ok=True)
for h in SOURCE.glob('*.pb.h'):shutil.copyfile(h,mesh/h.name)
(out/'CryptoEngine.h').write_text(r'''#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include "AES.h"
#include "SHA256.h"
#include "Curve25519.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/deviceonly.pb.h"
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define printBytes(...) ((void)0)
static constexpr size_t ACK_PROOF_SIZE=sizeof(meshtastic_Routing_ack_proof_t::bytes);
extern uint32_t fixtureExtra;
namespace HardwareRNG { inline bool fill(uint8_t *p,size_t n){if(n!=4)abort();memcpy(p,&fixtureExtra,4);return true;} }
struct FixtureRng {void rand(uint8_t *,size_t){abort();}};extern FixtureRng CryptRNG;
inline bool memfll(const void *p,unsigned char v,size_t n){const uint8_t *b=static_cast<const uint8_t *>(p);for(size_t i=0;i<n;++i)if(b[i]!=v)return false;return true;}
class CryptoEngine {
public:
  uint8_t private_key[32]{},shared_key[32]{},nonce[16]{};
  std::unique_ptr<BlockCipher> aes;
  bool encryptCurve25519(uint32_t,uint32_t,meshtastic_NodeInfoLite_public_key_t,uint64_t,size_t,const uint8_t *,uint8_t *);
  bool decryptCurve25519(uint32_t,meshtastic_NodeInfoLite_public_key_t,uint64_t,size_t,const uint8_t *,uint8_t *);
  bool ackProofCompute(const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *);
  void setDHPrivateKey(uint8_t *);void hash(uint8_t *,size_t);void aesSetKey(const uint8_t *,size_t);void aesEncrypt(uint8_t *,uint8_t *);
  bool setDHPublicKey(uint8_t *);void initNonce(uint32_t,uint64_t,uint32_t=0);
};extern CryptoEngine *crypto;
''')
for name in ['aes-ccm.cpp','aes-ccm.h']:shutil.copyfile(HERE/'reference'/name,out/name)
(out/'oracle.cpp').write_text('#include "CryptoEngine.h"\n#include "aes-ccm.h"\n'+constants+'\n'+'\n\n'.join(pieces)+r'''
uint32_t fixtureExtra=0;FixtureRng CryptRNG;CryptoEngine *crypto=nullptr;
extern "C" void stockPublic(const uint8_t *secret,uint8_t *pub){Curve25519::eval(pub,secret,nullptr);}
extern "C" bool stockEncrypt(const uint8_t *secret,const uint8_t *peer,uint32_t from,uint32_t id,uint32_t extra,const uint8_t *p,size_t n,uint8_t *out){
  CryptoEngine e;memcpy(e.private_key,secret,32);meshtastic_NodeInfoLite_public_key_t remote{};remote.size=32;memcpy(remote.bytes,peer,32);
  fixtureExtra=extra;crypto=&e;const bool ok=e.encryptCurve25519(42,from,remote,id,n,p,out);crypto=nullptr;return ok;
}
extern "C" bool stockDecrypt(const uint8_t *secret,const uint8_t *peer,uint32_t from,uint32_t id,const uint8_t *p,size_t n,uint8_t *out){
  CryptoEngine e;memcpy(e.private_key,secret,32);meshtastic_NodeInfoLite_public_key_t remote{};remote.size=32;memcpy(remote.bytes,peer,32);
  crypto=&e;const bool ok=e.decryptCurve25519(from,remote,id,n,p,out);crypto=nullptr;return ok;
}
extern "C" bool stockProof(const uint8_t *secret,const uint8_t *peer,uint32_t from,uint32_t to,uint32_t request,const uint8_t *routing,size_t n,uint8_t *out){
  CryptoEngine e;memcpy(e.private_key,secret,32);return e.ackProofCompute(peer,from,to,request,routing,n,out);
}
''')
ack_original=(HERE/'reference/AckProof.cpp').read_text();ack_pieces=[]
for marker in ['static bool isProvableAck(', 'bool ackProofExtract(', 'bool ackProofAttachWithKey(', 'AckProofResult ackProofVerifyWithKey(']:
    body,first,last=extract.extract(ack_original,marker);ack_pieces.append(body)
    sections.append({'source':'AckProof.cpp','function':marker,'first_line':first,'last_line':last,'sha256':hashlib.sha256(body.encode()).hexdigest()})
(out/'ack-oracle.cpp').write_text(r'''#include "CryptoEngine.h"
#include <pb_decode.h>
#include <pb_encode.h>
#define ACK_PROOF_FIELD_NUMBER meshtastic_Routing_ack_proof_tag
#define LOG_ERROR(...) ((void)0)
inline bool isBroadcast(uint32_t n){return n==UINT32_MAX;}
inline uint32_t getFrom(const meshtastic_MeshPacket *p){return p->from;}
int cryptLock=0;
namespace concurrency { struct LockGuard {explicit LockGuard(int &) {}}; }
enum class AckProofResult { ABSENT, VALID, INVALID, NO_KEY };
'''+ '\n\n'.join(ack_pieces)+r'''
extern "C" bool stockAckAttach(const uint8_t *secret,const uint8_t *peer,uint32_t from,uint32_t to,uint32_t request,const uint8_t *routing,size_t n,uint8_t *out,size_t *len){
  if(n>233)return false;CryptoEngine e;memcpy(e.private_key,secret,32);
  meshtastic_MeshPacket packet{};packet.from=from;packet.to=to;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  packet.decoded.portnum=meshtastic_PortNum_ROUTING_APP;packet.decoded.request_id=request;packet.decoded.payload.size=n;
  if(n)memcpy(packet.decoded.payload.bytes,routing,n);crypto=&e;const bool ok=ackProofAttachWithKey(&packet,peer);crypto=nullptr;
  if(ok){*len=packet.decoded.payload.size;memcpy(out,packet.decoded.payload.bytes,*len);}return ok;
}
extern "C" int stockAckVerify(const uint8_t *secret,const uint8_t *peer,uint32_t from,uint32_t to,uint32_t request,const uint8_t *routing,size_t n){
  if(n>233)return -1;CryptoEngine e;memcpy(e.private_key,secret,32);
  meshtastic_MeshPacket packet{};packet.from=from;packet.to=to;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  packet.decoded.portnum=meshtastic_PortNum_ROUTING_APP;packet.decoded.request_id=request;packet.decoded.payload.size=n;
  if(n)memcpy(packet.decoded.payload.bytes,routing,n);crypto=&e;const auto result=ackProofVerifyWithKey(&packet,request,peer);crypto=nullptr;
  return static_cast<int>(result);
}
''')
PKI=ROOT/'mt_only/src/pki';PKITEST=ROOT/'mt_only/tests/pki'
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-I'+str(HERE),'-I'+str(out),'-I'+str(out/'includes'),'-I'+str(SOURCE/'nanopb'),'-I'+str(MONO),'-I'+str(CRYPTO),'-I'+str(a.openssl/'include')]
flags+=['-I'+str(PKI)]
if a.psa_root:flags+=['-DMT_TEST_PSA','-I'+str(a.psa_library.parent.parent/'include'),'-I'+str(a.psa_root/'include'),'-I'+str(a.psa_root/'dispatch/include'),'-I'+str(a.psa_root/'drivers/builtin/include'),'-DTF_PSA_CRYPTO_CONFIG_FILE='+chr(34)+str(a.psa_config)+chr(34)]
commands=[];objects=[]
cc=shlex.split(os.environ.get('CC','clang'));cxx=shlex.split(os.environ.get('CXX','clang++'))
dead_strip='-Wl,--gc-sections' if platform.system()=='Linux' else '-Wl,-dead_strip'
def run(cmd):commands.append(cmd);subprocess.run(cmd,check=True)
obj=out/'monocypher.o';run([*cc,'-std=c11',*flags,'-c',str(MONO/'monocypher.c'),'-o',str(obj)]);objects.append(str(obj))
for name in ['Curve25519.cpp','BigNumberUtil.cpp','SHA256.cpp','Hash.cpp','Crypto.cpp','AESCommon.cpp','AES128.cpp','AES256.cpp','BlockCipher.cpp','Cipher.cpp']:
    obj=out/(name+'.o');run([*cxx,'-std=c++17',*flags,'-c',str(CRYPTO/name),'-o',str(obj)]);objects.append(str(obj))
for name in ['pb_common.c','pb_decode.c','pb_encode.c']:
    obj=out/(name+'.o');run([*cc,'-std=c11',*flags,'-c',str(SOURCE/'nanopb'/name),'-o',str(obj)]);objects.append(str(obj))
for name in ['oracle.cpp','aes-ccm.cpp','ack-oracle.cpp']:
    obj=out/(name+'.o');run([*cxx,'-std=c++17',*flags,'-c',str(out/name),'-o',str(obj)]);objects.append(str(obj))
candidate_sources=[HERE/'PkiCrypto.cpp',HERE/'PkiAck.cpp',PKI/'PkiSession.cpp']
if a.psa_root:candidate_sources+=[PKI/'PsaPkiBackend.cpp']
for source in candidate_sources:
    obj=out/(source.name+'.o');run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,'-Dcrypto_wipe=mt_pki_test_wipe','-c',str(source),'-o',str(obj)]);objects.append(str(obj))
test_sources=[HERE/'ack_tests.cpp',HERE/'tests.cpp',PKITEST/'session_tests.cpp']
if a.psa_root:test_sources+=[PKITEST/'psa_tests.cpp']
libs=[str(a.psa_library)] if a.psa_root else ['-L'+str(a.openssl/'lib'),'-lcrypto']
run([*cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,*map(str,test_sources),*objects,dead_strip,*libs,'-o',str(out/'tests')])
result=subprocess.run([str(out/'tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'tests.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr);result.check_returncode()
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'extraction-manifest.json').write_text(json.dumps({'commit':firmware['commit'],'functions_unmodified':True,'sections':sections,'stock_ccm':'entire unchanged aes-ccm.cpp with pinned Crypto AES256/SHA256/Curve25519','boundaries':'RNG replaced by deterministic PUBLIC nonce; class/packet environment shim; single-thread lock shim; no firmware lifecycle, hardware or peer trust'},indent=2)+'\n')
openssl_version='TF-PSA-Crypto PSA API (library supplied by caller; use tests/pki/run.py for pinned provenance)' if a.psa_root else subprocess.check_output([str(a.openssl/'bin/openssl'),'version'],text=True).strip()
(out/'results.json').write_text(json.dumps({'status':'PASS','ASAN_UBSAN':True,'cipher_vectors':681,'proof_vectors':234,'payload_tamper_cases':239,'normal_firmware_linked':False,'host_run_only':True,'hardware':False,'ack_wire_vectors':227,'ack_wire_mutations':72,'ack_parser_cases':20000,'session_tests':True,'psa_fault_tests':bool(a.psa_root),'component_wipes_instrumented':True,'backend':openssl_version,'source_files':{str(x.relative_to(ROOT)):sha(x) for x in [HERE/'PkiCrypto.h',HERE/'PkiCrypto.cpp',HERE/'PkiAck.h',HERE/'PkiAck.cpp',HERE/'ack_tests.cpp',HERE/'tests.cpp',HERE/'test.py',PKI/'PkiSession.h',PKI/'PkiSession.cpp',PKI/'PkiKeyProvider.h',PKI/'PsaPkiBackend.h',PKI/'PsaPkiBackend.cpp',PKITEST/'session_tests.cpp',PKITEST/'psa_tests.cpp']}},indent=2)+'\n')
