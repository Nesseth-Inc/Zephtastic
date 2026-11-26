#!/usr/bin/env python3
"""Offline signed NodeInfo tests against unchanged hash-locked stock XEdDSA."""
import argparse,hashlib,importlib.util,json,os,re,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT));import host_test_toolchain as host
APP=ROOT/'zephcore';SOURCE=APP/'bench/upstream-differential/source'
CRYPTO=SOURCE/'crypto-reference/Crypto';MONO=APP/'lib/monocypher'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    spec=importlib.util.spec_from_file_location('signature_reference',APP/'bench/run-mt-signature-tests.py')
    reference=importlib.util.module_from_spec(spec);spec.loader.exec_module(reference)
    firmware=reference.verify_lock(SOURCE/'source-lock.json',SOURCE)
    crypto=reference.verify_lock(SOURCE/'crypto-reference/crypto-source-lock.json',SOURCE/'crypto-reference')
    crcroot=SOURCE/'crypto-reference/ErriezCRC32';reference.verify_lock(crcroot/'source-lock.json',crcroot)
    generated=reference.oracle(out,firmware)
    with generated.open('a') as f:f.write(r'''
extern "C" void mtPinnedSignWithScalar(const uint8_t scalar[32],const uint8_t hedge[32],
    const uint8_t *message,size_t n,uint8_t signature[64]) {
    uint8_t copied[32],secret[32],ed[32];memcpy(copied,scalar,32);
    XEdDSA::priv_curve_to_ed_keys(copied,secret,ed);
    memset(signature,0,64);memcpy(signature,hedge,32);
    XEdDSA::sign(signature,secret,ed,message,n);
    clean(copied);clean(secret);clean(ed);
}
''')
    mesh=out/'includes/meshtastic';mesh.mkdir(parents=True,exist_ok=True)
    for h in SOURCE.glob('*.pb.h'):shutil.copyfile(h,mesh/h.name)
    includes=[ROOT/'mt_only/src',APP/'adapters/bench/vendor',MONO,CRYPTO,SOURCE,SOURCE/'nanopb',out/'includes',crcroot/'src']
    flags=['-g','-O1','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer',*[f'-I{x}' for x in includes],*host.crypto_compile_flags()]
    sources=[ROOT/'mt_only/src/pki/PkiSigner.cpp',ROOT/'mt_only/src/pki/PkiIdentity.cpp',ROOT/'mt_only/src/Protobuf.cpp',APP/'adapters/bench/vendor/MtSignature.cpp',APP/'adapters/bench/vendor/MtCodec.cpp',ROOT/'mt_only/tests/pki_signer_tests.cpp']
    tracked=[v for folder in (ROOT/'mt_only/src/pki',APP/'adapters/bench/vendor',MONO) for v in sorted(folder.glob('*')) if v.suffix in ('.h','.c','.cpp')]
    tracked += sources+[ROOT/'mt_only/scripts/test-pki-signer.py']
    before={str(v.relative_to(ROOT)):sha(v) for v in tracked}
    try:head=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True,stderr=subprocess.DEVNULL).strip() if (ROOT/'.git').exists() else None
    except (OSError,subprocess.CalledProcessError):head=None
    commands=[];objects=[]
    def run(cmd):commands.append(cmd);subprocess.run(cmd,check=True)
    c_sources=[MONO/'monocypher.c',MONO/'monocypher-ed25519.c',crcroot/'src/ErriezCRC32.c',*[SOURCE/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c')]]
    for src in c_sources:
        obj=out/(src.name+'.o');run([*host.cc(),'-std=c11',*flags,'-c',str(src),'-o',str(obj)]);objects.append(str(obj))
    for name in ['XEdDSA.cpp','Ed25519.cpp','Curve25519.cpp','SHA512.cpp','Hash.cpp','Crypto.cpp','BigNumberUtil.cpp']:
        extra=['-fno-sanitize=shift-base'] if name=='XEdDSA.cpp' else []
        obj=out/(name+'.o');run([*host.cxx(),'-std=c++17',*flags,*extra,'-c',str(CRYPTO/name),'-o',str(obj)]);objects.append(str(obj))
    signer=out/'PkiSigner.o'
    run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror',*flags,'-Dcrypto_wipe=mtSignerObservedWipe','-c',str(sources[0]),'-o',str(signer)])
    run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,*map(str,sources[1:]),str(generated),str(signer),*objects,host.dead_strip_flag(),*host.crypto_link_flags(),'-o',str(out/'signer-tests')])
    # Firmware toolchains disable exceptions/RTTI. Compile the producer that way too.
    run([*host.cxx(),'-std=c++17','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti',*flags,'-c',str(sources[0]),'-o',str(out/'signer-no-exceptions.o')])
    result=subprocess.run([str(out/'signer-tests')],capture_output=True,text=True,timeout=120,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
    (out/'tests.log').write_text(result.stdout+result.stderr);(out/'commands-private.json').write_text(json.dumps(commands,indent=2)+'\n')
    stable=all(sha(ROOT/name)==digest for name,digest in before.items())
    totals=re.search(r'PASS (\d+) checks, (\d+) exact stock signing vectors, (\d+) stock rejected mutations',result.stdout)
    wiped=re.search(r'PASS (\d+) candidate scratch wipes observed zero',result.stdout)
    report={'status':'PASS' if result.returncode==0 and stable and totals and wiped else 'FAIL','source_commit':head,'source_stable':stable,'exit_code':result.returncode,'source_sha256':before,'firmware_commit':firmware['commit'],'crypto_commit':crypto['commit'],'sanitizers':['address','undefined'],'reference_exception':'Only unchanged XEdDSA.cpp shift-base; candidate fully instrumented','hardware':False,'persistent_credentials':False,'checks':int(totals[1]) if totals else None,'stock_signing_vectors':int(totals[2]) if totals else None,'stock_rejected_mutations':int(totals[3]) if totals else None,'candidate_scratch_wipes_observed':int(wiped[1]) if wiped else None,'limits':['Synthetic fixture keys and entropy only','No device or radio interoperability','No stock-client or peer-trust authorization claim','No runtime integration or entropy provider']}
    (out/'RESULTS.json').write_text(json.dumps(report,indent=2)+'\n');print(result.stdout+result.stderr,end='')
    if report['status']!='PASS':raise SystemExit(result.returncode or 2)
if __name__=='__main__':main()
