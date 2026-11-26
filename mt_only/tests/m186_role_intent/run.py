#!/usr/bin/env python3
"""Compile/run M186 role-intent regression against an exact clean source head."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
from pathlib import PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def git(repo, *args):
    result = subprocess.run(['git', '-C', str(repo), *args], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
    if result.returncode:
        raise RuntimeError('git preflight failed: ' + result.stdout.strip())
    return result.stdout.strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='source worktree or exported source root')
    parser.add_argument('--expected-head', required=True, help='exact 40-hex source commit expected for this run')
    parser.add_argument('--output', required=True, type=Path, help='new output directory outside source tree')
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--c-compiler', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--local-clock', action='store_true', help='Test V25 XIAO clock with prior V22/V23/V24 intents')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9a-fA-F]{40}', args.expected_head):
        parser.error('--expected-head must be a full 40-character commit ID')
    repo = args.source.resolve(strict=True)
    app = repo / 'mt_only'
    proto = repo / 'zephcore/bench/upstream-differential/source'
    nanopb = proto / 'nanopb'
    vendor = repo / 'zephcore/adapters/bench/vendor'
    mono = repo / 'zephcore/lib/monocypher'
    output = args.output.resolve()
    if output.exists() or output.is_relative_to(repo):
        parser.error('--output must be a new directory outside the source worktree')
    case = HERE / 'cases.cpp'
    test_artifacts = [case, HERE / 'run.py', HERE / 'README.md']
    headers = [
        app / 'src/admin/RemoteDeviceTransaction.h', app / 'src/Config.h',
        app / 'src/ModuleSettings.h', app / 'src/ModuleJournal.h',
        app / 'src/SettingsJournal.h', app / 'src/modules/CompletionSettings.h',
        app / 'src/modules/CompletionJournal.h', app / 'src/notifications/NotificationSettings.h',
        app / 'src/notifications/NotificationJournal.h', app / 'src/network/MqttGatewaySettings.h',
        app / 'src/network/MqttGatewayJournal.h', app / 'src/network/MqttDefaultServerPolicy.h',
        app / 'src/PrimaryChannel.hpp', app / 'src/PskNormalizer.h', app / 'src/ChannelName.h',
        app / 'src/ChannelRecord.h', app / 'src/ChannelCandidates.h', app / 'src/ChannelOrder.h',
        app / 'src/RadioSettings.h', app / 'src/DeviceRolePolicy.h', app / 'src/HopPolicy.h',
        app / 'src/LoRaPresets.h', app / 'src/ManualLoRa.h', app / 'src/RegionPolicy.h',
        app / 'src/USChannel.h', app / 'src/gnss/GnssSettings.h', app / 'src/Protobuf.cpp',
        vendor / 'MtNodeInfo.h', vendor / 'MtCodec.h', vendor / 'MtKeyCommand.h', vendor / 'RelayCore.h',
        mono / 'monocypher.h', mono / 'monocypher.c',
    ]
    pb_headers = sorted(proto.glob('*.pb.h'))
    pb_sources = [nanopb / name for name in ('pb_common.c', 'pb_encode.c', 'pb_decode.c')]
    pb_headers += [nanopb / name for name in ('pb.h', 'pb_common.h', 'pb_encode.h', 'pb_decode.h')]
    source_inputs = headers + pb_headers + pb_sources
    if any(not path.is_file() for path in source_inputs + test_artifacts):
        parser.error('source worktree or test fixture is missing a required input')
    compiler = shlex.split(args.compiler)
    c_compiler = shlex.split(args.c_compiler)
    if not compiler or not c_compiler:
        parser.error('compiler command is empty')
    # Keep the selected driver name intact: clang++ may be a symlink to clang,
    # and resolving it changes C++ link behavior (standard library/ABI runtime).
    compiler_path = shutil.which(compiler[0])
    c_compiler_path = shutil.which(c_compiler[0])
    if compiler_path is None or c_compiler_path is None:
        parser.error('selected compiler executable was not found')
    if not Path(compiler_path).is_file() or not Path(c_compiler_path).is_file():
        parser.error('selected compiler executable is not a regular file')
    compiler[0] = str(Path(compiler_path).absolute())
    c_compiler[0] = str(Path(c_compiler_path).absolute())

    output.mkdir(parents=True)
    source_hashes = {str(path): sha(path) for path in source_inputs}
    test_hashes = {str(path): sha(path) for path in test_artifacts}
    summary = {
        'schema': 'm186-role-intent-native/v1', 'status': 'RUNNING',
        'expected_head': args.expected_head.lower(), 'target_build': False,
        'hardware': False, 'network': False, 'source_inputs_sha256': source_hashes,
        'test_artifacts_sha256': test_hashes, 'variants': [], 'failure': None,
    }
    (output / 'INPUTS.json').write_text(json.dumps({
        'expected_head': args.expected_head.lower(), 'source_inputs_sha256': source_hashes,
        'test_artifacts_sha256': test_hashes,
    }, indent=2, sort_keys=True) + '\n')
    phase = 'source-preflight'
    manifest_path = repo.parent / 'SOURCE-MANIFEST.json'
    manifest_hash = None
    manifest_files = {}
    try:
        if (repo / '.git').exists():
            source_kind = 'git'
            actual_head = git(repo, 'rev-parse', 'HEAD')
        else:
            source_kind = 'exported-manifest'
            raw_manifest = manifest_path.read_bytes()
            manifest_hash = hashlib.sha256(raw_manifest).hexdigest()
            manifest = json.loads(raw_manifest)
            actual_head = manifest.get('source_revision') if isinstance(manifest, dict) else None
            entries = manifest.get('files') if isinstance(manifest, dict) else None
            if not isinstance(actual_head, str) or not isinstance(entries, list):
                raise RuntimeError('source manifest must contain source_revision and files')
            if not re.fullmatch(r'[0-9a-fA-F]{40}', actual_head):
                raise RuntimeError('source manifest revision must be a full 40-character commit ID')
            for item in entries:
                if not isinstance(item, dict):
                    raise RuntimeError('source manifest entry must be an object')
                raw_path = item.get('path')
                digest = item.get('public_sha256')
                if not isinstance(raw_path, str) or not raw_path or '\\' in raw_path or '\0' in raw_path:
                    raise RuntimeError('source manifest contains an unsafe path')
                relative = PurePosixPath(raw_path)
                if (relative.is_absolute() or relative.as_posix() != raw_path
                        or any(part in ('', '.', '..') for part in relative.parts)):
                    raise RuntimeError('source manifest contains a non-normalized path: ' + raw_path)
                if raw_path in manifest_files:
                    raise RuntimeError('source manifest contains a duplicate path: ' + raw_path)
                if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', digest):
                    raise RuntimeError('source manifest contains an invalid SHA-256: ' + raw_path)
                resolved = (repo / raw_path).resolve()
                if not resolved.is_relative_to(repo):
                    raise RuntimeError('source manifest path escapes source root: ' + raw_path)
                if not resolved.is_file() or sha(resolved).lower() != digest.lower():
                    raise RuntimeError('source manifest file is missing or mismatched: ' + raw_path)
                manifest_files[raw_path] = digest.lower()
            manifest_selected = {path.resolve().relative_to(repo).as_posix() for path in source_inputs}
            if not manifest_selected.issubset(manifest_files):
                raise RuntimeError('source manifest omits a required fixture input')
            if {p.relative_to(repo).as_posix() for p in repo.rglob('*') if p.is_file()} != set(manifest_files):
                raise RuntimeError('export inventory differs from source manifest')
        summary['source_kind'] = source_kind
        summary['source_manifest_sha256'] = manifest_hash
        actual_head = str(actual_head)
        summary['actual_head'] = actual_head
        if actual_head.lower() != args.expected_head.lower():
            raise RuntimeError('source HEAD differs from --expected-head')
        if source_kind == 'git':
            status = git(repo, 'status', '--porcelain=v1', '--untracked-files=all')
            if status:
                raise RuntimeError('source worktree is dirty; refusing to compile')
        gen = output / 'generated/meshtastic'
        gen.mkdir(parents=True)
        for header in proto.glob('*.pb.h'):
            shutil.copyfile(header, gen / header.name)

        def compile_or_run(command, label, timeout):
            nonlocal phase
            phase = label
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            logs.append(shlex.join(command))
            logs.append(result.stdout)
            (build / 'build.log').write_text('\n'.join(logs))
            if result.returncode:
                raise RuntimeError(f'{label} failed with exit {result.returncode}')
            return result

        for name, extra in [('default-enums', []), ('short-enums', ['-fshort-enums'])]:
            if args.local_clock: extra = extra + ['-DCONFIG_MT_LOCAL_CLOCK=1']
            phase = name
            build = output / name
            build.mkdir()
            logs = []
            includes = [f'-I{app / "src"}', f'-I{vendor}', f'-I{mono}', f'-I{output / "generated"}', f'-I{nanopb}']
            sanitizers = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-O1', '-Wall', '-Wextra', '-Werror']
            objects = []
            for index, source in enumerate([*pb_sources, mono / 'monocypher.c']):
                obj = build / f'c-{index}.o'
                command = [*c_compiler, '-std=c11', *sanitizers, *extra, *includes,
                           '-c', str(source), '-o', str(obj)]
                compile_or_run(command, f'{name}:c{index}', 180)
                objects.append(obj)
            for index, source in enumerate([app / 'src/Protobuf.cpp', case]):
                obj = build / f'cpp-{index}.o'
                command = [*compiler, '-std=c++17', *sanitizers, *extra, *includes,
                           '-c', str(source), '-o', str(obj)]
                compile_or_run(command, f'{name}:cpp{index}', 180)
                objects.append(obj)
            binary = build / 'role-intent-cases'
            link = [*compiler, '-std=c++17', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    *extra, *map(str, objects), '-o', str(binary)]
            compile_or_run(link, f'{name}:link', 180)
            result = compile_or_run([str(binary)], f'{name}:test', 60)
            logs_text = '\n'.join(logs)
            (build / 'build.log').write_text(logs_text)
            terminal = [line for line in result.stdout.splitlines() if line.startswith('SUMMARY ')]
            if len(terminal) != 1 or terminal[0] != f'SUMMARY groups=12 checks={1524 if args.local_clock else 1461} failures=0':
                raise RuntimeError(f'{name}: fixture summary differs from the expected historical-intent result')
            summary['variants'].append({'name': name, 'status': 'PASS', 'summary': terminal[0],
                                        'binary_sha256': sha(binary),
                                        'log_sha256': sha(build / 'build.log')})
        phase = 'final-source-integrity'
        if source_kind == 'git':
            if git(repo, 'rev-parse', 'HEAD').lower() != args.expected_head.lower():
                raise RuntimeError('source HEAD changed during fixture run')
            if git(repo, 'status', '--porcelain=v1', '--untracked-files=all'):
                raise RuntimeError('source worktree became dirty during fixture run')
        else:
            final_manifest = manifest_path.read_bytes()
            if hashlib.sha256(final_manifest).hexdigest() != manifest_hash:
                raise RuntimeError('source manifest changed during fixture run')
            if {p.relative_to(repo).as_posix() for p in repo.rglob('*') if p.is_file()} != set(manifest_files):
                raise RuntimeError('export inventory changed during fixture run')
            final_manifest_object = json.loads(final_manifest)
            if final_manifest_object.get('source_revision', '').lower() != args.expected_head.lower():
                raise RuntimeError('source manifest revision changed during fixture run')
            for relative, digest in manifest_files.items():
                if sha((repo / relative).resolve()).lower() != digest:
                    raise RuntimeError('manifest-listed source changed during fixture run: ' + relative)
        if source_hashes != {str(path): sha(path) for path in source_inputs}:
            raise RuntimeError('production input hash changed during fixture run')
        if test_hashes != {str(path): sha(path) for path in test_artifacts}:
            raise RuntimeError('test artifact hash changed during fixture run')
        summary['status'] = 'PASS'
        summary['source_unchanged'] = True
    except BaseException as error:
        summary['status'] = 'FAIL'
        summary['source_unchanged'] = False
        summary['failure'] = {'phase': phase, 'type': type(error).__name__, 'message': str(error)}
        raise
    finally:
        summary['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (output / 'RESULT.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'status': summary['status'], 'variants': len(summary['variants']),
                      'source_unchanged': summary.get('source_unchanged', False)}))

if __name__ == '__main__':
    if not __debug__:
        raise RuntimeError('optimized driver refused')
    main()
