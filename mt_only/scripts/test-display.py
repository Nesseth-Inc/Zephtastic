#!/usr/bin/env python3
"""Exercise the actual display backend with host devices; no hardware access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
app = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
proto = app.parent / 'zephcore/bench/upstream-differential/source'
generated_headers = out / 'protobuf' / 'meshtastic'
generated_headers.mkdir(parents=True, exist_ok=True)
for header in proto.glob('*.pb.h'):
    shutil.copyfile(header, generated_headers / header.name)
includes = ['-I' + str(out / 'protobuf'), '-I' + str(proto / 'nanopb'),
            '-I' + str(app.parent / 'zephcore/lib/monocypher')]
source = (app / 'src/ui/Display.cpp').read_text()
body = source[source.index('namespace mt::ui {'):]
fixture = (app / 'tests/display_runtime_fixture.cpp').read_text()
marker = '// INCLUDE_ACTUAL_DISPLAY_BODY'
if fixture.count(marker) != 1:
    raise RuntimeError('Display test fixture marker must appear exactly once')
generated = out / 'display-runtime.cpp'
generated.write_text(fixture.replace(marker, body))
reports = []
cxx = shlex.split(os.environ.get('CXX', 'g++'))
for name, width, height, mono, rows in [('compact', 160, 80, 0, 80), ('compact-strips', 160, 80, 0, 16), ('mono', 128, 64, 1, 64), ('wide-strips', 240, 135, 0, 16)]:
    exe = out / name
    command = [*cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-g', '-I' + str(app / 'src'), *includes,
               '-I' + str(app.parent / 'zephcore/adapters/bench/vendor'),
               '-DCONFIG_MT_DISPLAY=1', '-DCONFIG_MT_T096_OWNER_TX=1',
               '-DCONFIG_MT_UI_DEVICE_LABEL="HELTEC T096"',
               f'-DCONFIG_MT_UI_WIDTH={width}', f'-DCONFIG_MT_UI_HEIGHT={height}',
               f'-DCONFIG_MT_UI_MONO={mono}', f'-DCONFIG_MT_UI_STRIP_ROWS={rows}',
               *(['-DCONFIG_MT_XIAO_EXPANSION_OLED=1', '-DCONFIG_SSD1306_DEFAULT_CONTRAST=128'] if mono else []),
               str(generated), '-o', str(exe)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(exe)], text=True, capture_output=True,
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0',
                                 'UBSAN_OPTIONS': 'halt_on_error=1'})
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    print(name, result.stdout + result.stderr)
    result.check_returncode()
    reports.append({'profile': name, 'PASS': True, 'command': command})
(out / 'results.json').write_text(json.dumps({
    'PASS': True, 'ASAN_UBSAN': True, 'hardware': False,
    'production_source_sha256': hashlib.sha256(source.encode()).hexdigest(),
    'production_body_sha256': hashlib.sha256(body.encode()).hexdigest(),
    'checks': ['splash transition and explicit command render', 'configured blank with stock zero/always-on values', 'button and admin wake without navigation',
               'second action navigation', 'text wake/dedup', 'write/blank failures and unavailable refusal',
               'bounded failed wake/init cleanup', 'uptime wrap',
               'descriptor and strip-buffer bounds', 'boot/live display preferences', 'one-day timeout through wrap',
               'root carousel state and framebuffer change', 'splash/detail/sleep/manual reset semantics',
               'one-step no-catch-up and uint32 rollover',
               'XIAO OLED OTA frame and wake after successful ESP handoff; unsupported, failed and pending updates keep the display untouched'],
    'profiles': reports}, indent=2) + '\n')
