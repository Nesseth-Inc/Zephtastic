#!/usr/bin/env python3
"""Host tests for stock-compatible channel AES-CCM; no RF/device access."""
import argparse, hashlib, json, os, shlex, shutil, subprocess, sys
from pathlib import Path

PIN = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--stock-root', required=True, type=Path)
p.add_argument('--short-enums', action='store_true')
a = p.parse_args()
app = Path(__file__).resolve().parents[1]
repo = app.parent
stock = a.stock_root.resolve()
out = a.output.resolve()
assert subprocess.check_output(['git', '-C', str(stock), 'rev-parse', 'HEAD'], text=True).strip() == PIN
assert not subprocess.check_output(['git', '-C', str(stock), 'status', '--porcelain=v1', '--untracked-files=all'], text=True)
out.mkdir(parents=True, exist_ok=False)
stock_copy = out / 'stock-ccm'
stock_copy.mkdir()
for name in ('aes-ccm.cpp', 'aes-ccm.h'):
    shutil.copyfile(stock / 'src/mesh' / name, stock_copy / name)
    assert (stock_copy/name).read_bytes() == (stock/'src/mesh'/name).read_bytes()
stub = stock_copy / 'CryptoEngine.h'
stub.write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
class CryptoEngine {
  uint8_t key_[32]{};
  size_t keyLen_=0;
public:
  void aesSetKey(const uint8_t *key,size_t keyLen);
  void aesEncrypt(uint8_t *input,uint8_t *output);
};
extern CryptoEngine *crypto;
''')
proto = repo / 'zephcore/bench/upstream-differential/source'
vendor = repo / 'zephcore/adapters/bench/vendor'
case = app / 'tests/channel_aead_cases.cpp'
sources = [Path(__file__).resolve(), case, app/'src/Config.h', app/'src/ChannelRecord.h',
           app/'src/PrimaryChannel.hpp', app/'src/ChannelName.h', app/'src/PskNormalizer.h',
           app/'src/ChannelCandidates.h', app/'src/ChannelOrder.h', app/'src/RadioSettings.h',
           app/'src/LoRaPresets.h', app/'src/DeviceRolePolicy.h', app/'src/HopPolicy.h',
           app/'src/gnss/GnssSettings.h', vendor/'MtNodeInfo.h', vendor/'MtChannelAead.h',
           vendor/'MtCodec.h', vendor/'MtCodec.cpp', stock/'src/mesh/aes-ccm.cpp',
           stock/'src/mesh/aes-ccm.h', stub]
pins = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
flags = ['-fsanitize=address,undefined', '-g', '-O2', '-I'+str(app/'src'), '-I'+str(app/'tests'),
         '-I'+str(vendor), '-I'+str(stock_copy), '-I'+str(proto/'nanopb')]
if a.short_enums:
    flags += ['-fshort-enums']
if sys.platform.startswith('linux'):
    flags += ['-DMT_HOST_OPENSSL=1']
cxx = shlex.split(os.environ.get('CXX', 'clang++'))
commands = []
objects = []
for name, source in (('channel-aead-cases', case), ('mt-codec', vendor/'MtCodec.cpp'),
                     ('stock-aes-ccm', stock_copy/'aes-ccm.cpp')):
    obj = out/(name+'.o')
    cmd = [*cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *flags]
    if name == 'stock-aes-ccm':
        # Preserve strict warnings on the port and test. The pinned donor has
        # one known unused L parameter; suppress only that warning for its TU.
        cmd.append('-Wno-unused-parameter')
    cmd += ['-c', str(source), '-o', str(obj)]
    subprocess.run(cmd, check=True, cwd=out, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
    commands.append(cmd)
    objects.append(str(obj))
cmd = [*cxx, '-fsanitize=address,undefined', '-g', '-O2', *objects]
if sys.platform.startswith('linux'):
    cmd += ['-lcrypto']
cmd += ['-o', str(out/'channel-aead-tests')]
commands.append(cmd)
env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'}
subprocess.run(cmd, check=True, cwd=out, env=env)
run = subprocess.run([str(out/'channel-aead-tests')], cwd=out, capture_output=True, text=True, env=env)
(out/'test.log').write_text(run.stdout+run.stderr)
print(run.stdout+run.stderr, end='')
run.check_returncode()
groups = [line for line in run.stdout.splitlines() if line.startswith('PASS ') and 'suite groups=' not in line]
assert len(groups) == 5, f'expected 5 scenario groups, got {len(groups)}'
assert 'PASS channel AEAD suite groups=5' in run.stdout
assert all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest for path, digest in pins.items())
(out/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
(out/'RESULT.json').write_text(json.dumps({
    'status': 'PASS', 'groups': groups, 'ASAN_UBSAN': True, 'short_enums': a.short_enums,
    'stock_revision': PIN, 'source_sha256': pins, 'hardware': False, 'RF': False,
    'scope': 'Bounded channel CCM helper against exact stock aes-ccm.cpp; codec wire and Config AEAD mode. No channel RF interoperability or target crypto-backend proof.'
}, indent=2)+'\n')
