#!/usr/bin/env python3
"""Compile the pinned MLX90632 stock driver and its EnvironmentMetrics writer."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
app = here.parents[1]
sys.path.insert(0, str(app.parent))
import host_test_toolchain as host

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock', type=Path, required=True)
parser.add_argument('--emotibit', type=Path, required=True)
args = parser.parse_args()
stock_pin = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
driver_pin = '388e79f4621c6127c3e9621f01e30783e16e2a9c'

def source(repo, pin, path):
    return subprocess.check_output(['git', '-C', repo, 'show', pin + ':' + path], text=True)

with tempfile.TemporaryDirectory(prefix='zt-mlx90632-') as temp:
    out = Path(temp)
    stem = 'SparkFun_MLX90632_Arduino_Library'
    for suffix in ('.cpp', '.h'):
        (out/(stem+suffix)).write_text(source(args.emotibit, driver_pin, 'src/'+stem+suffix))
    path = 'src/modules/Telemetry/Sensor/MLX90632Sensor.cpp'
    module = source(args.stock, stock_pin, path)
    start = module.index('bool MLX90632Sensor::getMetrics(')
    writer = module[start:module.index('\n}',start)+2]
    (out/'oracle.h').write_text("""
#include "SparkFun_MLX90632_Arduino_Library.h"
struct Reading {
  struct {struct {bool has_temperature;float temperature;} environment_metrics;} variant;
};
using meshtastic_Telemetry=Reading;
struct MLX90632Sensor {MLX90632 mlx;bool getMetrics(Reading*);} stock;
""" + writer)
    for abi in ([], ['-fshort-enums']):
        subprocess.run([*host.cxx(), '-std=c++17', '-DARDUINO=100', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *abi,
                        '-I'+str(here/'mocks'), '-I'+str(out), '-I'+str(app/'src'),
                        here/'cases.cpp', out/(stem+'.cpp'), '-o', out/'cases'], check=True)
        subprocess.run([out/'cases'], check=True, env={**os.environ, 'UBSAN_OPTIONS':'halt_on_error=1'})
