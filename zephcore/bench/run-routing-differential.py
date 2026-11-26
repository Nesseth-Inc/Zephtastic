#!/usr/bin/env python3
"""Offline oracle: compile exact pinned whole-function slices, never rewritten decisions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

APP = Path(__file__).resolve().parent.parent
sys.path.insert(0,str(APP.parent));import host_test_toolchain as host
ROOT = APP / 'bench/upstream-differential'
SOURCE = ROOT / 'source'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def closing_brace(text, opening):
    """Find a C++ block end while excluding comments, strings and character literals."""
    depth, index, state = 0, opening, 'code'
    while index < len(text):
        c, following = text[index], text[index:index + 2]
        if state == 'line':
            if c == '\n': state = 'code'
        elif state == 'comment':
            if following == '*/': state = 'code'; index += 1
        elif state in ('"', "'"):
            if c == '\\': index += 1
            elif c == state: state = 'code'
        elif following == '//': state = 'line'; index += 1
        elif following == '/*': state = 'comment'; index += 1
        elif c in ('"', "'"): state = c
        elif c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return index + 1
        index += 1
    raise ValueError('Unclosed source block')

def extract(text, marker):
    matches = list(re.finditer(r'^' + re.escape(marker), text, re.M))
    if len(matches) != 1: raise ValueError(f'{marker}: expected exactly one definition')
    begin = matches[0].start()
    end = closing_brace(text, text.index('{', begin))
    return text[begin:end], text[:begin].count('\n') + 1, text[:end].count('\n') + 1

SLICES = {
    'NextHopRouter.cpp': [
        'bool NextHopRouter::relayOpaquePacket(', 'bool NextHopRouter::opaqueWasSeenRecently(',
        'PendingPacket::PendingPacket(', 'ErrorCode NextHopRouter::send(',
        'ErrorCode NextHopRouter::sendWithNextHop(', 'bool NextHopRouter::shouldFilterReceived(',
        'void NextHopRouter::sniffReceived(', 'bool NextHopRouter::perhapsRebroadcast(',
        'std::optional<uint8_t> NextHopRouter::getNextHop(',
        'PendingPacket *NextHopRouter::findPendingPacket(', 'bool NextHopRouter::stopRetransmission(NodeNum',
        'bool NextHopRouter::roleAllowsCancelingFromTxQueue(', 'bool NextHopRouter::stopRetransmission(GlobalPacketId',
        'PendingPacket *NextHopRouter::startRetransmission(', 'int32_t NextHopRouter::doRetransmissions(',
        'void NextHopRouter::setNextTx(', 'RouteHealth *NextHopRouter::findRouteHealth(',
        'RouteHealth *NextHopRouter::getOrAllocRouteHealth(', 'void NextHopRouter::noteRouteLearned(',
        'void NextHopRouter::noteRouteSuccess(', 'void NextHopRouter::noteRouteFailure(',
        'bool NextHopRouter::isRouteStale(', 'void NextHopRouter::clearRouteHealth('],
    'FloodingRouter.cpp': [
        'ErrorCode FloodingRouter::send(', 'bool FloodingRouter::shouldFilterReceived(',
        'bool FloodingRouter::perhapsHandleUpgradedPacket(', 'bool FloodingRouter::reprocessPacket(',
        'bool FloodingRouter::roleAllowsCancelingDupe(', 'void FloodingRouter::perhapsCancelDupe(',
        'bool FloodingRouter::isRebroadcaster(', 'void FloodingRouter::sniffReceived('],
    'ReliableRouter.cpp': [
        'void ReliableRouter::perhapsGenerateImplicitAckForOwnOverheard(',
        'bool ReliableRouter::shouldSuccessAckWithWantAck(', 'bool ReliableRouter::shouldFilterReceived('],
    'Router.cpp': ['bool Router::shouldDecrementHopLimit(',
                   'static bool routingAuthCacheMatches(', 'static void storeRoutingAuthCache(',
                   'static bool applyRoutingAuthCache(', 'static void clearRoutingAuthCache(',
                   'static bool canonicalSignableSize(', 'static NodeInfoBootstrapResult verifyFirstContactNodeInfo(',
                   'bool checkXeddsaReceivePolicy(', 'RoutingAuthVerdict passesRoutingAuthGate('],
    'RadioInterface.cpp': ['uint32_t RadioInterface::getRetransmissionMsec(',
                          'uint32_t RadioInterface::computeSlotTimeMsec(',
                          'uint32_t RadioInterface::getPacketTime(const meshtastic_MeshPacket *',
                          'uint8_t RadioInterface::getCWsize(',
                          'uint32_t RadioInterface::getTxDelayMsecWeightedWorst(',
                          'bool RadioInterface::shouldRebroadcastEarlyLikeRouter(',
                          'uint32_t RadioInterface::getTxDelayMsecWeighted('],
    'NodeDB.cpp': ['NodeNum getFrom(', 'bool isFromUs(', 'bool isToUs(', 'bool isBroadcast(', 'int8_t getHopsAway('],
}

def generate(out):
    lock = json.loads((SOURCE / 'source-lock.json').read_text())
    for item in lock['files']:
        path = SOURCE / item['local_file']
        assert sha(path.read_bytes()) == item['sha256'], f'Pinned source modified: {path}'
    crypto_reference = SOURCE / 'crypto-reference'
    for lock_name in ['firmware-metadata-lock.json', 'crypto-source-lock.json']:
        dependency_lock = json.loads((crypto_reference / lock_name).read_text())
        for item in dependency_lock['files']:
            path = crypto_reference / item['local_file']
            assert sha(path.read_bytes()) == item['sha256'], f'Pinned dependency modified: {path}'
    crc_reference = crypto_reference / 'ErriezCRC32'
    for item in json.loads((crc_reference / 'source-lock.json').read_text())['files']:
        path = crc_reference / item['local_file']
        assert sha(path.read_bytes()) == item['sha256'], f'Pinned CRC dependency modified: {path}'
    inc = out / 'includes/meshtastic'; inc.mkdir(parents=True, exist_ok=True)
    for path in SOURCE.glob('*.pb.h'): shutil.copyfile(path, inc / path.name)
    pieces, manifest = [], []
    auth_declarations=[]
    router_header=(SOURCE/'Router.h').read_text()
    for marker in ['enum DecodeState','enum class RoutingAuthVerdict']:
        section,first,last=extract(router_header,marker)
        auth_declarations.append(section+';')
        manifest.append({'source':'Router.h','type':marker,'first_line':first,'last_line':last,'compiled_sha256':sha(section.encode())})
    device_header=(SOURCE/'deviceonly.pb.h').read_text()
    public_key_declaration=re.search(r'^typedef PB_BYTES_ARRAY_T\(32\) meshtastic_NodeInfoLite_public_key_t;',device_header,re.M).group(0)
    auth_declarations.append(public_key_declaration)
    declaration_begin=device_header.index(public_key_declaration)
    manifest.append({'source':'deviceonly.pb.h','type':'meshtastic_NodeInfoLite_public_key_t',
                     'first_line':device_header[:declaration_begin].count('\n')+1,
                     'last_line':device_header[:declaration_begin+len(public_key_declaration)].count('\n')+1,
                     'compiled_sha256':sha(public_key_declaration.encode())})
    (out/'upstream-auth-declarations.inc').write_text('\n'.join(auth_declarations)+'\n')
    router_source=(SOURCE/'Router.cpp').read_text()
    cache_begin=router_source.index('struct RoutingAuthCache {')
    cache_end=router_source.index('static uint32_t routingAuthEvaluations;',cache_begin)+len('static uint32_t routingAuthEvaluations;')
    cache_section=router_source[cache_begin:cache_end]
    pieces.append(cache_section)
    manifest.append({'source':'Router.cpp','coverage':'RoutingAuthCache type and file-static storage','first_line':router_source[:cache_begin].count('\n')+1,
                     'last_line':router_source[:cache_end].count('\n')+1,'compiled_sha256':sha(cache_section.encode())})
    bootstrap_enum,first,last=extract(router_source,'enum class NodeInfoBootstrapResult')
    pieces.append(bootstrap_enum+';')
    manifest.append({'source':'Router.cpp','type':'enum class NodeInfoBootstrapResult','first_line':first,'last_line':last,'compiled_sha256':sha(bootstrap_enum.encode())})
    packet_history = (SOURCE / 'PacketHistory.cpp').read_text()
    # Full implementation; only environment-owned #include directives are omitted.
    stripped = re.sub(r'^#include[^\n]*\n', '', packet_history, flags=re.M)
    pieces.append(stripped)
    manifest.append({'source': 'PacketHistory.cpp', 'coverage': 'entire implementation',
                     'transformation': 'remove #include directives only', 'compiled_sha256': sha(stripped.encode())})
    packet_header = (SOURCE / 'PacketHistory.h').read_text()
    stripped_header = re.sub(r'^#include[^\n]*\n', '', packet_header, flags=re.M)
    (out / 'upstream-packet-history.inc').write_text(stripped_header)
    manifest.append({'source': 'PacketHistory.h', 'coverage': 'entire declaration',
                     'transformation': 'remove #include directives only', 'compiled_sha256': sha(stripped_header.encode())})
    for filename, markers in SLICES.items():
        original = (SOURCE / filename).read_text()
        for marker in markers:
            section, first, last = extract(original, marker)
            pieces.append(section)
            manifest.append({'source': filename, 'function': marker, 'first_line': first,
                             'last_line': last, 'compiled_sha256': sha(section.encode())})
    structs = (SOURCE / 'NextHopRouter.h').read_text()
    definitions = []
    for marker in ['struct GlobalPacketId', 'struct PendingPacket', 'struct RouteHealth']:
        section, first, last = extract(structs, marker)
        definitions.append(section + ';')
        manifest.append({'source': 'NextHopRouter.h', 'type': marker, 'first_line': first,
                         'last_line': last, 'compiled_sha256': sha(section.encode())})
    (out / 'upstream-structs.inc').write_text('\n\n'.join(definitions) + '\n')
    router_source = (SOURCE / 'Router.cpp').read_text()
    header_begin = router_source.index('    if (isBroadcast(p->to))\n        p->want_ack = false;')
    header_marker = '    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum()); // set the relayer to us'
    header_end = router_source.index(header_marker, header_begin) + len(header_marker)
    header_slice = router_source[header_begin:header_end]
    (out / 'upstream-send-header.inc').write_text(header_slice + '\n')
    manifest.append({'source': 'Router.cpp', 'coverage': 'Router::send header egress statements',
                     'first_line': router_source[:header_begin].count('\n') + 1,
                     'last_line': router_source[:header_end].count('\n') + 1,
                     'compiled_sha256': sha(header_slice.encode())})
    oracle = out / 'upstream-oracle.cpp'
    helpers='''
void fixtureAuthReset() {
    static concurrency::Lock fixtureCacheLock;
    routingAuthCacheLock=&fixtureCacheLock;
    routingAuthEvaluations=0;
    clearRoutingAuthCache();
}
uint32_t fixtureAuthEvaluationCount() {return routingAuthEvaluations;}
bool fixtureAuthCacheValid() {return routingAuthCache.valid;}
bool fixtureApplyAuthCache(meshtastic_MeshPacket *p) {return applyRoutingAuthCache(p);}
'''
    oracle.write_text('#include "Fixture.h"\n\n' + '\n\n'.join(pieces) + '\n'+helpers)
    (out / 'extraction-manifest.json').write_text(json.dumps({'commit': lock['commit'],
        'functions_unmodified': True, 'sections': manifest}, indent=2) + '\n')
    return oracle, manifest

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('/tmp/zephcore-routing-differential'))
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    (out / 'results.json').write_text(json.dumps({'status': 'RUNNING'}, indent=2) + '\n')
    oracle, manifest = generate(out)
    binary = out / 'routing-differential'
    cmd = [*host.cxx(), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
           '-Wno-unused-variable', '-fsanitize=address,undefined', '-g', '-O1']
    for directory in [ROOT, ROOT / 'stubs', SOURCE, SOURCE / 'nanopb', out, out / 'includes', APP / 'adapters/bench']:
        cmd += ['-I', str(directory)]
    cmd += [str(oracle), str(ROOT / 'routing_differential.cpp'), '-o', str(binary)]
    (out / 'compile-command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    candidate_paths = [APP / 'adapters/bench/vendor/RoutingCore.h', APP / 'adapters/bench/vendor/RelayCore.h',
                       APP / 'adapters/bench/vendor/MtCodec.h']
    candidate_hashes = {path: sha(path.read_bytes()) for path in candidate_paths}
    subprocess.run(cmd, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True,
        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}, timeout=120)
    (out / 'routing-differential.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end=''); print(result.stderr, end='')
    result.check_returncode()
    assert all(sha(path.read_bytes()) == digest for path, digest in candidate_hashes.items()), \
        'Candidate source changed during compile/test; rerun against a stable snapshot'
    result_lines = [line[7:] for line in result.stdout.splitlines() if line.startswith('RESULT ')]
    assert len(result_lines) == 1, 'Missing structured comparison counts'
    counts = json.loads(result_lines[0])
    report = {'status': 'PASS', 'upstream_commit': '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c',
              'actual_upstream_implementation_compiled': True, 'mechanically_extracted_sections': len(manifest),
              'checks': counts,
              'sanitizers': ['address', 'undefined'], 'hardware_or_crypto_qualification': False,
              'actual_upstream_routing_auth_decisions_compiled': True,
              'actual_xeddsa_crypto_compiled_in_this_runner': False,
              'candidate_signature_verdicts_injected': True,
              'candidate_signature_verifier_linked_in_this_runner': False,
              'fixture_boundaries': ['packet-pool ownership and synthetic send queue', 'controlled monotonic clock',
                 'bounded NodeDB known/direct/ambiguous neighbor fixture', 'airtime and channel utilization inputs',
                 'controlled perhapsDecode verdict and decoded metadata; decryption is outside this runner',
                 'authoritative/opportunistic/warm-key NodeDB lookups and first-contact node allocation fixtures',
                 'XEdDSA verification verdict stub; no real signing keys or cryptographic verification',
                 'protobuf canonical sizing, first-contact User decoding and CRC32 results are controlled inputs',
                 'lock-count fixture validates call paths but does not qualify concurrency or RTOS locking',
                 'no real USB, SPI, IRQ, RF, RTOS, private keys or endpoint delivery receipts']}
    report['compiled_candidate_sources'] = [
        {'path': str(path.relative_to(APP)), 'sha256': digest}
        for path, digest in candidate_hashes.items()]
    report['reference_source_lock_sha256'] = sha((SOURCE / 'source-lock.json').read_bytes())
    report['fixture_source_sha256'] = sha((ROOT / 'routing_differential.cpp').read_bytes())
    report['fixture_input_sha256'] = {
        filename: sha((ROOT / filename).read_bytes())
        for filename in ['routing_differential.cpp', 'Fixture.h', 'FixtureTypes.h']}
    report['runner_sha256'] = sha(Path(__file__).read_bytes())
    report['extraction_manifest_sha256'] = sha((out / 'extraction-manifest.json').read_bytes())
    report['retained_crypto_dependency'] = {
        'commit': '1c817c2f27aa4593e07d0f7e13b3dbf8d980bcae',
        'firmware_metadata_lock_sha256': sha((SOURCE / 'crypto-reference/firmware-metadata-lock.json').read_bytes()),
        'source_lock_sha256': sha((SOURCE / 'crypto-reference/crypto-source-lock.json').read_bytes()),
        'crc_tag': '1.0.1', 'crc_commit': '0bd91795ee09a4268b9b4a0abcd1394677f419c1',
        'crc_source_lock_sha256': sha((SOURCE / 'crypto-reference/ErriezCRC32/source-lock.json').read_bytes()),
        'compiled_by_this_runner': False}
    report['documented_policy_differences'] = [
        'candidate coalesces same-key opaque originator copies while one is queued; upstream queues every originator repeat; later unqueued repeats are admitted without a decoded ARQ cap',
        'opaque table 64 slots with 60-second expiry and fail-closed capacity; upstream 32-slot FIFO eviction',
        'candidate decoded history, pending, neighbor and routes have explicit fixed caps and expiry',
        'candidate relay ARQ timer starts at actual transmit completion; upstream schedules at initial enqueue',
        'candidate cleans pending immediately at final transmit completion; upstream deletes at following deadline',
        'candidate cancellation requires matching recipient or known observed/named intermediate; upstream request-ID cancellation is broader',
        'candidate non-originating payload changes cannot upgrade/cancel without verified signature; actual upstream admission decisions use controlled crypto verdicts here',
        'candidate decode-failed transit remains isolated opaque, matching upstream hash-collision fallback; local backend failure drops',
        'candidate unverified full signatures are isolated opaque; upstream accepts unknown-key full signatures in Compatible/Balanced and rejects known-invalid or Strict unknown-key signatures',
        'candidate verified full signatures enter decoded routing and local signatureRejected drops before history; those local verdicts are explicitly injected, with real verification tested separately',
        'candidate has no Strict/Balanced downgrade-policy implementation; this runner exercises those actual upstream policy decisions as explicit unsupported boundaries',
        'only unfavorited ROUTER and CLIENT relay policies; no ROUTER_LATE/favorite hop preservation or endpoint ACK/PKI generation',
        'candidate actual received plaintext size conservatively exceeds canonical nanopb re-encoding when redundant/unknown fields occur']
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))

if __name__ == '__main__': main()
