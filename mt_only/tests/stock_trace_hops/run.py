#!/usr/bin/env python3
"""Compare real pinned HopScaling, TraceRoute and AirTime procedures with ZT."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

PIN = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', source.index('\n{', start))+2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    args = parser.parse_args()
    def donor(path):
        return subprocess.check_output(['git', '-C', args.stock, 'show', PIN+':src/'+path], text=True)

    with tempfile.TemporaryDirectory(prefix='zt-trace-hop-') as tmp:
        out = Path(tmp)
        source = ROOT/'zephcore/bench/upstream-differential/source'
        (out/'meshtastic').symlink_to(source, target_is_directory=True)
        (out/'HopScalingModule.cpp').write_text(donor('modules/HopScalingModule.cpp'))
        (out/'HopScalingModule.h').write_text(donor('modules/HopScalingModule.h'))
        for stub in ('concurrency/LockGuard.h','mesh/mesh-pb-constants.h','mesh-pb-constants.h','SafeFile.h','FSCommon.h',
                     'NodeDB.h','SPILock.h','airtime.h','meshUtils.h','configuration.h'):
            f=out/stub
            f.parent.mkdir(exist_ok=True,parents=True)
            f.touch()
        (out/'mesh/Default.h').write_text('\n'.join(l for l in donor('mesh/Default.h').splitlines()
                                                   if l.startswith('#define default_hop_scaling_')))
        (out/'MeshTypes.h').write_text('#pragma once\n#include <meshtastic/mesh.pb.h>\n'
            '#include <meshtastic/config.pb.h>\n#define HOP_MAX 7\n#define LOG_INFO(...) ((void)0)\n'
            '#define LOG_WARN(...) ((void)0)\nstruct StockConfig {meshtastic_Config_DeviceConfig device{};}; extern StockConfig config;\n')
        (out/'concurrency/OSThread.h').write_text('#pragma once\nnamespace concurrency { struct OSThread {'
            'OSThread(const char*){} void setIntervalFromNow(unsigned){} virtual int32_t runOnce()=0; }; }')
        trace=donor('modules/TraceRouteModule.cpp')
        (out/'trace.inc').write_text('\n'.join(method(trace,'void TraceRouteModule::'+name+'(')
                                              for name in ('insertUnknownHops','appendMyIDandSNR')))
        (out/'distance.inc').write_text(method(donor('mesh/NodeDB.cpp'),'int8_t getHopsAway('))
        airtime=donor('airtime.cpp')
        (out/'fold.inc').write_text(method(airtime,'void AirTime::Windows::foldChannelUtil('))
        for short in (False,True):
            flags=['-fshort-enums'] if short else []
            cmd=[*shlex.split(os.environ.get('CXX','c++')), '-std=c++17','-O1','-fsanitize=address,undefined',
                 *flags,'-DHAS_VARIABLE_HOPS=1','-DPIO_UNIT_TESTING','-I',out,'-I',source/'nanopb',
                 '-I',ROOT/'mt_only/src',HERE/'cases.cpp',out/'HopScalingModule.cpp','-o',out/'trace-hop-check']
            subprocess.run(cmd,check=True)
            subprocess.run([out/'trace-hop-check'],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0',
                                                                   'UBSAN_OPTIONS':'halt_on_error=1'})
if __name__=='__main__':
    main()
