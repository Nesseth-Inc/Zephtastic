#!/usr/bin/env python3
"""Independent offline protocol probes using byte-locked stock source oracles."""
import argparse, hashlib, importlib.util, json, os, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--stock-build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--openssl',type=Path,default=Path('/opt/homebrew/opt/openssl@3'))
a=p.parse_args();root=a.source.resolve();stock=a.stock_build.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(root));import host_test_toolchain as host
(out/'RESULTS.json').unlink(missing_ok=True)
here=Path(__file__).resolve().parent;proto=root/'zephcore/bench/upstream-differential/source';mono=root/'zephcore/lib/monocypher';vendor=root/'zephcore/adapters/bench/vendor'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
for item in json.loads((proto/'source-lock.json').read_text())['files']:
    assert sha(proto/item['local_file'])==item['sha256']
spec=importlib.util.spec_from_file_location('extract',root/'zephcore/bench/run-routing-differential.py')
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
original=(proto/'CryptoEngine.cpp').read_text();body,first,last=extract.extract(original,'static size_t buildSigningBuffer(')
header=(proto/'CryptoEngine.h').read_text()
constants='\n'.join(line for line in header.splitlines() if line.startswith(('static constexpr ','#define ')) and ('XEDDSA_SIGN' in line) and 'XEDDSA_SIGN_BUF_LEN' not in line)
builder=out/'signing-oracle.cpp'
builder.write_text('#include "CryptoEngine.h"\n'+constants+'\nstatic void putLE32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}\n'+body+'\nsize_t stockSigningBuffer(uint8_t *p,size_t cap,uint32_t f,uint32_t id,uint32_t to,const meshtastic_Data *d){return buildSigningBuffer(p,cap,f,id,to,d);}\n')
flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer']
flags+=['-I'+str(x) for x in (stock,stock/'includes',mono,vendor,proto/'crypto-reference/Crypto',proto/'nanopb',root/'mt_only/experimental/pki',a.openssl/'include')]
commands=[]
def run(cmd):
    commands.append(cmd)
    subprocess.run(cmd,check=True)
obj=out/'monocypher-ed25519.o';run([*host.cc(),'-std=c11',*flags,'-c',str(mono/'monocypher-ed25519.c'),'-o',str(obj)])
baseline=json.loads((stock/'results.json').read_text());assert baseline['status']=='PASS'
objects=[x for x in stock.glob('*.o') if x.name not in ('PkiSession.cpp.o','PkiAck.cpp.o','PkiCrypto.cpp.o','PsaPkiBackend.cpp.o')]
candidate_inputs=[root/'mt_only/experimental/pki'/name for name in ('PkiCrypto.h','PkiCrypto.cpp','PkiAck.h','PkiAck.cpp')]+[vendor/'MtSignature.h',vendor/'MtSignature.cpp']
before={str(x.relative_to(root)):sha(x) for x in candidate_inputs}
for name in ('PkiCrypto.cpp','PkiAck.cpp'):
    target=out/(name+'.o');run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,'-c',str(root/'mt_only/experimental/pki'/name),'-o',str(target)]);objects.append(target)
cmd=[*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,str(here/'m10_protocol_boundaries.cpp'),str(builder),str(vendor/'MtSignature.cpp'),str(obj),*map(str,objects),host.dead_strip_flag(),'-L'+str(a.openssl/'lib'),'-lcrypto','-o',str(out/'protocol-tests')]
run(cmd)
r=subprocess.run([str(out/'protocol-tests')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
stable=before=={str(x.relative_to(root)):sha(x) for x in candidate_inputs}
receipt={'status':'PASS' if r.returncode==0 and stable else 'FAIL','exit_code':r.returncode,'ASAN_UBSAN':True,'source_stable':stable,'candidate_sources':before,'stock_baseline_receipt_sha256':sha(stock/'results.json'),'stock_extraction_manifest_sha256':sha(stock/'extraction-manifest.json'),'stock_commit':json.loads((proto/'source-lock.json').read_text())['commit'],'hardware':False,'live_keys':False,'groups':r.stdout.splitlines(),'actual_stock_signing_builder':{'first_line':first,'last_line':last,'sha256':hashlib.sha256(body.encode()).hexdigest()},'test_hashes':{x.name:sha(x) for x in (here/'m10_protocol_boundaries.cpp',Path(__file__),builder)},'linked_object_sha256':{x.name:sha(x) for x in objects},'fixture_boundaries':['PUBLIC deterministic scalar fixtures only','stock RNG supplied deterministic extra nonce','host OpenSSL SHA256/HMAC for candidate ACK adapter','no runtime lifecycle or radio qualification']}
(out/'RESULTS.json').write_text(json.dumps(receipt,indent=2)+'\n')
raise SystemExit(r.returncode if stable else 2)
