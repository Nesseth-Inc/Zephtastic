#!/usr/bin/env python3
"""Offline macOS codec tests against exact pinned nanopb and crypto adapters."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

APP = Path(__file__).resolve().parent.parent
SOURCE = APP / 'bench/upstream-differential/source'
sys.path.insert(0, str(APP.parent))
import host_test_toolchain as host

def sha(data):
    return hashlib.sha256(data).hexdigest()

def verify_stock(stock):
    stock=Path(stock).resolve()
    stock_oid=subprocess.check_output(['git','-C',str(stock),'rev-parse','HEAD'],text=True).strip()
    if stock_oid!='8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c':
        raise RuntimeError('Wrong stock oracle Git OID: '+stock_oid)
    stock_status=subprocess.check_output(
        ['git','-C',str(stock),'status','--porcelain','-uall'],text=True)
    if stock_status:
        raise RuntimeError('Stock oracle tree has tracked or untracked drift; refusing compile')
    return stock_oid

def generate_oracle(out, lock):
    # Reuse the lexical whole-function extractor, never edit upstream decisions.
    spec = importlib.util.spec_from_file_location('routing_extract', APP / 'bench/run-routing-differential.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    pieces, sections = [], []
    original = (SOURCE / 'CryptoEngine.cpp').read_text()
    for marker in ['void CryptoEngine::setKey(', 'void CryptoEngine::encryptPacket(', 'void CryptoEngine::initNonce(']:
        section, first, last = module.extract(original, marker)
        pieces.append(section)
        sections.append({'source': 'CryptoEngine.cpp', 'function': marker,
                         'first_line': first, 'last_line': last, 'compiled_sha256': sha(section.encode())})
    shutil.copyfile(SOURCE / 'ESP32CryptoEngine.cpp', out / 'ESP32CryptoEngine.cpp')
    sections.append({'source': 'ESP32CryptoEngine.cpp', 'coverage': 'entire unchanged implementation',
                     'compiled_sha256': sha((out / 'ESP32CryptoEngine.cpp').read_bytes())})
    (out / 'CryptoEngine.h').write_text(r'''#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define MAX_BLOCKSIZE 256
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
struct CryptoKey { uint8_t bytes[32]; int8_t length; };
class CryptoEngine {
public:
    uint8_t nonce[16]{};
    CryptoKey key{};
    virtual ~CryptoEngine() = default;
    virtual void encryptAESCtr(CryptoKey,uint8_t *,size_t,uint8_t *)=0;
    void initNonce(uint32_t,uint64_t,uint32_t=0);
    void encryptPacket(uint32_t,uint64_t,size_t,uint8_t *);
    void setKey(const CryptoKey &);
};
''')
    (out / 'configuration.h').write_text('#pragma once\n')
    shim = out / 'mbedtls'
    shim.mkdir(exist_ok=True)
    # The oracle uses the platform's native CTR operation, deliberately a
    # different implementation path than the candidate's ECB-generated blocks.
    if sys.platform == 'darwin':
        native = r'''#include <CommonCrypto/CommonCryptor.h>
#define MT_NATIVE_CONTEXT CCCryptorRef
static int mt_native_ctr(const uint8_t *key,size_t keyLen,const uint8_t counter[16],const uint8_t *input,uint8_t *output,size_t n) {
    CCCryptorRef cryptor=nullptr;size_t moved=0;
    CCCryptorStatus result=CCCryptorCreateWithMode(kCCEncrypt,kCCModeCTR,kCCAlgorithmAES,ccNoPadding,
        counter,key,keyLen,nullptr,0,0,kCCModeOptionCTR_BE,&cryptor);
    if(result==kCCSuccess) result=CCCryptorUpdate(cryptor,input,n,output,n,&moved);
    if(cryptor) CCCryptorRelease(cryptor);
    return result==kCCSuccess&&moved==n?0:-1;
}'''
        boundary = 'minimal class environment + CommonCrypto native CTR mbedTLS shim'
    elif sys.platform.startswith('linux'):
        native = r'''#include <openssl/evp.h>
#define MT_NATIVE_CONTEXT EVP_CIPHER_CTX*
static int mt_native_ctr(const uint8_t *key,size_t keyLen,const uint8_t counter[16],const uint8_t *input,uint8_t *output,size_t n) {
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();int moved=0,final=0;
    const EVP_CIPHER *cipher=keyLen==16?EVP_aes_128_ctr():EVP_aes_256_ctr();
    const bool ok=ctx&&EVP_EncryptInit_ex(ctx,cipher,nullptr,key,counter)==1&&
        EVP_EncryptUpdate(ctx,output,&moved,input,int(n))==1&&
        EVP_EncryptFinal_ex(ctx,output+moved,&final)==1&&size_t(moved+final)==n;
    EVP_CIPHER_CTX_free(ctx);return ok?0:-1;
}'''
        boundary = 'minimal class environment + OpenSSL native CTR mbedTLS shim'
    else:
        raise RuntimeError(f'No reviewed native CTR oracle for {sys.platform}')
    (shim / 'aes.h').write_text(r'''#pragma once
#include <cassert>
#include <cstring>
''' + native + r'''
struct mbedtls_aes_context { uint8_t key[32]; size_t keyLen; };
static bool mtShimOk=true;
static void mbedtls_aes_init(mbedtls_aes_context *c) { memset(c,0,sizeof(*c)); }
static void mbedtls_aes_free(mbedtls_aes_context *c) { memset(c,0,sizeof(*c)); }
static int mbedtls_aes_setkey_enc(mbedtls_aes_context *c,const uint8_t *key,unsigned bits) {
    assert(bits==128||bits==256);c->keyLen=bits/8;memcpy(c->key,key,c->keyLen);return 0;
}
static int mbedtls_aes_crypt_ctr(mbedtls_aes_context *c,size_t n,size_t *offset,
    uint8_t counter[16],uint8_t *,const uint8_t *input,uint8_t *output) {
    assert(*offset==0);mtShimOk=mt_native_ctr(c->key,c->keyLen,counter,input,output,n)==0;
    return mtShimOk?0:-1;
}
''')
    oracle = out / 'pinned-crypto-oracle.cpp'
    oracle.write_text('#include "ESP32CryptoEngine.cpp"\n\n' + '\n\n'.join(pieces) + r'''
extern "C" void mtPinnedNonce(uint32_t from,uint32_t id,uint8_t out[16]) {
    crypto->initNonce(from,id);memcpy(out,crypto->nonce,16);
}
extern "C" bool mtPinnedCrypt(const uint8_t *key,size_t keyLen,uint32_t from,uint32_t id,uint8_t *p,size_t n) {
    if(!key||(keyLen!=16&&keyLen!=32)||!p||n>239) return false;
    CryptoKey k{};memcpy(k.bytes,key,keyLen);k.length=int8_t(keyLen);
    mtShimOk=false;crypto->setKey(k);crypto->encryptPacket(from,id,n,p);return mtShimOk;
}
''')
    manifest = {'commit': lock['commit'], 'functions_unmodified': True, 'sections': sections,
                'host_boundary': boundary,
                'target_mbedtls_or_psa_runtime_tested': False}
    (out / 'crypto-extraction-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return oracle

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--stock-root', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stock=args.stock_root.resolve()
    stock_oid=verify_stock(stock)
    generated=stock/'src/mesh/generated/meshtastic'
    descriptor_names=('mesh.pb.cpp','remote_hardware.pb.cpp','xmodem.pb.cpp','config.pb.cpp',
                      'module_config.pb.cpp','channel.pb.cpp','device_ui.pb.cpp','telemetry.pb.cpp')
    protobuf_sources=[generated/name for name in descriptor_names]
    if not all(path.is_file() for path in protobuf_sources) or sha((generated/'mesh.pb.cpp').read_bytes())!=sha((SOURCE/'mesh.pb.cpp').read_bytes()):
        raise RuntimeError('Pinned generated protobuf source set changed')
    lock = json.loads((SOURCE / 'source-lock.json').read_text())
    for entry in lock['files']:
        path = SOURCE / entry['local_file']
        if sha(path.read_bytes()) != entry['sha256']:
            raise RuntimeError(f'Pinned reference changed: {path}')
    includes = out / 'includes/meshtastic'
    includes.mkdir(parents=True, exist_ok=True)
    for header in SOURCE.glob('*.pb.h'):
        shutil.copyfile(header, includes / header.name)
    protobuf_headers=sorted(generated.glob('*.pb.h'))
    for header in protobuf_headers:
        destination=includes/header.name
        if destination.exists() and sha(destination.read_bytes())!=sha(header.read_bytes()):
            raise RuntimeError('Local pinned header differs from exact stock tree: '+header.name)
        if not destination.exists():shutil.copyfile(header,destination)
    oracle = generate_oracle(out, lock)
    common = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
              '-ffunction-sections', '-fdata-sections', '-I' + str(out / 'includes'),
              '-I' + str(out), '-I' + str(SOURCE / 'nanopb'), *host.crypto_compile_flags()]
    commands, objects = [], []
    for name in ['pb_common', 'pb_encode', 'pb_decode']:
        obj = out / (name + '.o')
        command = [*host.cc(), '-std=c11', *common, '-c', str(SOURCE / 'nanopb' / (name + '.c')), '-o', str(obj)]
        commands.append(command)
        subprocess.run(command, check=True)
        objects.append(str(obj))
    binary = out / 'mt-codec-tests'
    command = [*host.cxx(), '-std=c++17', '-Wall', '-Wextra', '-Werror', *common,
               '-DMT_CODEC_NANOPB_REFERENCE', '-DMT_CODEC_UPSTREAM_CRYPTO_REFERENCE',
               str(APP / 'adapters/bench/vendor/MtCodec.cpp'), str(APP / 'bench/tests/mt_codec_tests.cpp'),
               *map(str,protobuf_sources), str(oracle), *objects, host.dead_strip_flag(),
               *host.crypto_link_flags(), '-o', str(binary)]
    commands.append(command)
    (out / 'compile-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120,
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
    (out / 'mt-codec-tests.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    print(result.stderr, end='')
    result.check_returncode()
    report = {'status': 'PASS', 'upstream_commit': lock['commit'],
              'sanitizers': ['address', 'undefined'], 'groups': 8,
              'native_ctr_adapter_differential_cases': 5000, 'generated_valid_protobuf_cases': 4990,
              'random_malformed_protobuf_cases': 100000,
              'reference_crypto_functions_unmodified': True,
              'host_crypto_boundary': 'native CTR shim for unchanged pinned ESP32 adapter; candidate uses ECB',
              'stock_git_oid':stock_oid,
              'stock_tree_clean':True,
              'protobuf_source_sha256':{path.name:sha(path.read_bytes()) for path in protobuf_sources},
              'protobuf_header_sha256':{path.name:sha(path.read_bytes()) for path in protobuf_headers},
              'keys': 'public NIST vectors and deterministic synthetic fixture bytes only',
              'hardware_or_radio_actions': False, 'target_psa_runtime_qualified': False,
              'limitations': ['CTR channels only; no AEAD/CCM mode', 'no PKI endpoint decryption',
                              'XEdDSA verified in separate signature runner; no pairwise ACK-proof verification',
                              'decoded protobuf alone is not authentication',
                              'zero-hop ACK encoder unconnected to radio or core',
                              'empty or licensed User does not establish known-node evidence',
                              'malformed Routing protobuf rejected even if outer Data parses']}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))

if __name__ == '__main__':
    main()
