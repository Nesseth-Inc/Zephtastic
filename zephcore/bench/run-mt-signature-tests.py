#!/usr/bin/env python3
"""Offline public-signature tests using unchanged pinned upstream XEdDSA."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import subprocess
import sys

APP = Path(__file__).resolve().parent.parent
sys.path.insert(0,str(APP.parent));import host_test_toolchain as host
SOURCE = APP / 'bench/upstream-differential/source'
CRYPTO = SOURCE / 'crypto-reference/Crypto'
MONO = APP / 'lib/monocypher'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def verify_lock(path, root):
    lock = json.loads(path.read_text())
    for entry in lock['files']:
        local = root / entry['local_file']
        if sha(local.read_bytes()) != entry['sha256']:
            raise RuntimeError(f'Pinned source changed: {local}')
    return lock

def oracle(out, firmware):
    spec = importlib.util.spec_from_file_location('routing_extract', APP / 'bench/run-routing-differential.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    original = (SOURCE / 'CryptoEngine.cpp').read_text()
    sections, pieces = [], []
    for marker in ['static void putLE32(', 'static size_t buildSigningBuffer(', 'void CryptoEngine::curve_to_ed_pub(']:
        body, first, last = module.extract(original, marker)
        pieces.append(body)
        sections.append({'source': 'CryptoEngine.cpp', 'function': marker,
                         'first_line': first, 'last_line': last, 'compiled_sha256': sha(body.encode())})
    header = (SOURCE/'CryptoEngine.h').read_text()
    constants = '\n'.join(line for line in header.splitlines() if
        line.startswith(('#define XEDDSA_SIGNING_VERSION ',
                         '#define XEDDSA_SIGNED_FLAG_WANT_RESPONSE ',
                         '#define XEDDSA_SIGNED_FLAG_HAS_BITFIELD ',
                         'static constexpr size_t XEDDSA_SIGNED_HEADER_LEN ')))
    sections.append({'source':'CryptoEngine.h','coverage':'unchanged signing-format constants',
                     'compiled_sha256':sha(constants.encode())})
    prefix = r'''#include "MtCodec.h"
#include "meshtastic/mesh.pb.h"
#include "XEdDSA.h"
#include "Curve25519.h"
#include "Crypto.h"
#include <cstring>
class CryptoEngine { public: static void curve_to_ed_pub(const uint8_t *,uint8_t *); };
'''
    wrappers = r'''
extern "C" size_t mtPinnedSigningBuffer(uint32_t from,uint32_t id,uint32_t to,
    const bench::RouteMetadata *m,uint8_t *out,size_t cap) {
    meshtastic_Data d=meshtastic_Data_init_zero;
    d.portnum=static_cast<meshtastic_PortNum>(m->portnum);
    d.request_id=m->request_id; d.reply_id=m->reply_id; d.emoji=m->emoji;
    d.has_bitfield=m->hasBitfield; d.bitfield=m->bitfield; d.want_response=m->wantResponse;
    d.payload.size=m->payloadLength; memcpy(d.payload.bytes,m->payload,m->payloadLength);
    return buildSigningBuffer(out,cap,from,id,to,&d);
}
extern "C" void mtPinnedCurveToEd(const uint8_t curve[32],uint8_t ed[32]) {
    CryptoEngine::curve_to_ed_pub(curve,ed);
}
extern "C" bool mtPinnedSignatureVerify(const uint8_t curve[32],const uint8_t sig[64],
    const uint8_t *message,size_t n) {
    uint8_t ed[32]; CryptoEngine::curve_to_ed_pub(curve,ed);
    return XEdDSA::verify(sig,ed,message,n);
}
extern "C" void mtPinnedFixtureKey(unsigned seed,uint8_t curve[32],uint8_t edSecret[32],uint8_t ed[32]) {
    // Deterministic PUBLIC test fixtures only. Never an endpoint provisioner.
    uint8_t scalar[32],base[32]={9};
    for(unsigned i=0;i<32;++i) scalar[i]=uint8_t(seed*29u+i*17u+3u);
    scalar[0]&=0xf8; scalar[31]&=0x7f; scalar[31]|=0x40;
    Curve25519::eval(curve,scalar,base);
    XEdDSA::priv_curve_to_ed_keys(scalar,edSecret,ed);
    clean(scalar);
}
extern "C" void mtPinnedFixtureSign(const uint8_t edSecret[32],const uint8_t ed[32],
    const uint8_t *message,size_t n,uint8_t sig[64]) {
    for(unsigned i=0;i<64;++i) sig[i]=uint8_t(i*11u+n);
    XEdDSA::sign(sig,edSecret,ed,message,n);
}
'''
    generated = out / 'pinned-signature-oracle.cpp'
    generated.write_text(prefix+constants+'\n\n'+'\n\n'.join(pieces)+wrappers)
    manifest = {'firmware_commit': firmware['commit'], 'functions_unmodified': True,
                'sections': sections, 'crypto_dependency': '1c817c2f27aa4593e07d0f7e13b3dbf8d980bcae',
                'crypto_boundary': 'unchanged upstream SHA512/Ed25519/XEdDSA/Curve25519 implementations',
                'test_private_scalars': 'deterministic public host fixtures only; no endpoint keys'}
    (out / 'signature-extraction-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return generated

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('/tmp/zephcore-mt-signature-tests'))
    args = parser.parse_args()
    out=args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    firmware=verify_lock(SOURCE/'source-lock.json', SOURCE)
    crypto=verify_lock(SOURCE/'crypto-reference/crypto-source-lock.json', SOURCE/'crypto-reference')
    crcroot=SOURCE/'crypto-reference/ErriezCRC32'
    crc=verify_lock(crcroot/'source-lock.json', crcroot)
    generated=oracle(out, firmware)
    mono_hashes = {name: sha((MONO/name).read_bytes()) for name in
                   ['monocypher.c', 'monocypher.h', 'monocypher-ed25519.c', 'monocypher-ed25519.h']}
    for name in mono_hashes:
        if 'Monocypher version 4.0.3' not in (MONO/name).read_text().splitlines()[0]:
            raise RuntimeError('Monocypher version changed: '+name)
    includes = ['-I'+str(APP/'adapters/bench/vendor'), '-I'+str(MONO), '-I'+str(CRYPTO),
                '-I'+str(SOURCE), '-I'+str(SOURCE/'nanopb')]
    # Generated headers use meshtastic/ includes, supplied without edits.
    mesh=out/'includes/meshtastic'; mesh.mkdir(parents=True, exist_ok=True)
    for header in SOURCE.glob('*.pb.h'):
        (mesh/header.name).write_bytes(header.read_bytes())
    includes.append('-I'+str(out/'includes'))
    includes.append('-I'+str(crcroot/'src'))
    common=['-g', '-O1', '-ffunction-sections', '-fdata-sections',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *includes, *host.crypto_compile_flags()]
    objects=[]; commands=[]
    for name in ['monocypher.c', 'monocypher-ed25519.c']:
        obj=out/(name+'.o'); cmd=[*host.cc(), '-std=c11', *common, '-c', str(MONO/name), '-o', str(obj)]
        commands.append(cmd); subprocess.run(cmd,check=True); objects.append(str(obj))
    crcobj=out/'ErriezCRC32.o'
    cmd=[*host.cc(),'-std=c11',*common,'-c',str(crcroot/'src/ErriezCRC32.c'),'-o',str(crcobj)]
    commands.append(cmd);subprocess.run(cmd,check=True);objects.append(str(crcobj))
    for name in ['XEdDSA.cpp', 'Ed25519.cpp', 'Curve25519.cpp', 'SHA512.cpp', 'Hash.cpp', 'Crypto.cpp', 'BigNumberUtil.cpp']:
        # The unchanged SUPERCOP/ref10 field code shifts negative signed limbs
        # (e.g. XEdDSA.cpp:303). Preserve that finding; exempt only shift-base
        # instrumentation in this reference TU. Candidate and Monocypher retain
        # every ASAN/UBSAN check. Reference source itself is never patched.
        reference_flags=['-fno-sanitize=shift-base'] if name=='XEdDSA.cpp' else []
        obj=out/(name+'.o'); cmd=[*host.cxx(), '-std=c++17', *common, *reference_flags,
                                '-c', str(CRYPTO/name), '-o', str(obj)]
        commands.append(cmd); subprocess.run(cmd,check=True); objects.append(str(obj))
    binary=out/'mt-signature-tests'
    cmd=[*host.cxx(), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', *common,
         str(APP/'adapters/bench/vendor/MtCodec.cpp'), str(APP/'adapters/bench/vendor/MtSignature.cpp'),
         str(APP/'bench/tests/mt_signature_tests.cpp'), str(generated), *objects,
         host.dead_strip_flag(), *host.crypto_link_flags(), '-o', str(binary)]
    commands.append(cmd)
    (out/'compile-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    subprocess.run(cmd,check=True)
    run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=120,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
    (out/'mt-signature-tests.log').write_text(run.stdout+run.stderr)
    print(run.stdout,end=''); print(run.stderr,end=''); run.check_returncode()
    totals=re.search(r'PASS (\d+) checks, (\d+) exact upstream XEdDSA signatures; metadata=(\d+)B cache=(\d+)B',run.stdout)
    if not totals: raise RuntimeError('Missing signature fixture totals')
    report={'status':'PASS','firmware_commit':firmware['commit'],
            'crypto_dependency_commit':crypto.get('commit','1c817c2f27aa4593e07d0f7e13b3dbf8d980bcae'),
            'crc_dependency_commit':crc['commit'],
            'checks':int(totals[1]),'actual_upstream_signatures':int(totals[2]),
            'signing_buffer_positive_vectors':64,'signed_field_mutations':768,
            'metadata_bytes':int(totals[3]),'cache_bytes':int(totals[4]),
            'sanitizers':['address','undefined'],'monocypher_version':'4.0.3','monocypher_sha256':mono_hashes,
            'reference_sanitizer_exception':'XEdDSA.cpp shift-base only: negative signed-limb shifts in unchanged ref10 source',
            'hardware_or_radio_actions':False,'endpoint_key_creation':False,
            'private_fixture_material':'deterministic public test scalars, host-only',
            'limits':['COMPATIBLE relay subset; no Balanced/Strict policy parity',
                      'unknown signed non-NodeInfo quarantined opaque',
                      'canonical/weak X25519 key admission stricter than upstream conversion',
                      'Monocypher Ed25519 rejects noncanonical S; upstream verifier may accept S+L',
                      '30min public authority TTL and 32 entries; no live eviction',
                      'CRC32/node binding is not a certificate and does not authenticate relay header',
                      'no endpoint PKI decrypt, pairwise ACK proof verify or endpoint receipt synthesis']}
    (out/'results.json').write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report))

if __name__=='__main__': main()
