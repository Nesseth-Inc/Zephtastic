#!/usr/bin/env python3
"""Fault-test exact supplied Zephyr implementation functions; no device access."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:ssize_t|int|uint16_t|void)\s+' + name
                      + r'\([^;]*?\)\s*\{', source, re.M | re.S)
    if not match:
        raise RuntimeError('function not found: ' + name)
    at, depth = match.end(), 1
    # The selected pinned functions have no brace characters inside string literals.
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[match.start():at]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nvs', type=pathlib.Path, required=True,
                        help='Zephyr subsys/settings/src/settings_nvs.c')
    parser.add_argument('--store', type=pathlib.Path, required=True,
                        help='Zephyr subsys/settings/src/settings_store.c')
    parser.add_argument('--app', type=pathlib.Path, required=True, help='mt_only directory')
    parser.add_argument('--kvss', type=pathlib.Path,
                        help='optional Zephyr subsys/kvss/nvs/nvs.c for startup regressions')
    parser.add_argument('--output', type=pathlib.Path,
                        help='optional new/existing directory for generated files; defaults to temporary directory')
    args = parser.parse_args()
    for key in ('nvs', 'store', 'app', 'kvss'):
        value = getattr(args, key)
        if value is not None:
            setattr(args, key, value.resolve())
    compiler = shlex.split(os.environ.get('CXX', 'clang++'))
    nvs, store = args.nvs.read_text(), args.store.read_text()
    parts = [(HERE / 'preamble.cpp').read_text()]
    for name in ('settings_nvs_read_fn', 'settings_nvs_cache_add',
                 'settings_nvs_cache_match', 'settings_nvs_load',
                 'settings_nvs_save', 'settings_nvs_backend_init'):
        parts.append(function(nvs, name))
    parts.append('struct default_param {void *buf; size_t buf_len; size_t *val_len;};')
    for name in ('settings_set_default_cb', 'settings_get_val_len_default_cb',
                 'settings_get_val_len', 'settings_load_one', 'settings_save_one'):
        parts.append(function(store, name))
    parts.append((HERE / 'cases.cpp').read_text())
    settings_source = '\n\n'.join(parts)
    jobs = [('settings-cache0', settings_source, ['-DCONFIG_SETTINGS_NVS_NAME_CACHE=0']),
            ('settings-cache1', settings_source, ['-DCONFIG_SETTINGS_NVS_NAME_CACHE=1'])]
    if args.kvss:
        jobs.append(('nvs-startup', '\n\n'.join([
            (HERE / 'startup_preamble.cpp').read_text(),
            function(args.kvss.read_text(), 'nvs_startup'),
            (HERE / 'startup_cases.cpp').read_text()]), []))
    results = []
    with tempfile.TemporaryDirectory(prefix='mt-nvs-review-') as temporary:
        output = args.output.resolve() if args.output else pathlib.Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        for name, source, defines in jobs:
            generated = output / (name + '.cpp')
            executable = output / name
            generated.write_text(source)
            command = compiler + ['-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter',
                '-Wno-unused-function', '-fsanitize=address,undefined', '-g'] + defines + [
                '-I' + str(HERE / 'include'), '-I' + str(args.app / 'src'),
                '-I' + str(args.app.parent / 'zephcore/adapters/bench/vendor'),
                str(generated), '-o', str(executable)]
            subprocess.run(command, check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})
            log = result.stdout + result.stderr
            print(name, 'exit', result.returncode)
            print(log, end='')
            (output / (name + '.log')).write_text(log)
            results.append({'name': name, 'exit': result.returncode})
        # Deliberately omit absolute paths and compiler commands from public evidence.
        inputs = {'settings_nvs.c': args.nvs, 'settings_store.c': args.store,
                  'ZephyrSettings.h': args.app / 'src/ZephyrSettings.h',
                  'SettingsJournal.h': args.app / 'src/SettingsJournal.h'}
        if args.kvss:
            inputs['nvs.c'] = args.kvss
        report = {'source_sha256': {name: digest(path) for name, path in inputs.items()},
                  'results': results, 'sanitizers': ['address', 'undefined'],
                  'hardware': False, 'rf': False}
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report))
    return int(any(result['exit'] for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
