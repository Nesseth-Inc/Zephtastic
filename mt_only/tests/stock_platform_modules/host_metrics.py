#!/usr/bin/env python3
"""Compare a Linux proc fixture with stock HostMetrics and serialize it with the stock Python client."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from meshtastic.mesh_interface import MeshInterface
from meshtastic.protobuf import mesh_pb2, portnums_pb2, telemetry_pb2

APP = Path(__file__).resolve().parents[2]
PIN = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
spec = importlib.util.spec_from_file_location('zt_host_metrics', APP / 'host/host_metrics.py')
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)


class SerialCapture:
    sendData = MeshInterface.sendData
    def _generatePacketId(self):
        return 103
    def _sendPacket(self, packet, to, **kwargs):
        assert to == '^all' and not kwargs['wantAck']
        self.packet = packet


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    stock = parser.parse_args().stock
    src = subprocess.check_output(['git', '-C', stock, 'show', PIN + ':src/modules/Telemetry/HostMetrics.cpp'], text=True)
    start = src.index('meshtastic_Telemetry HostMetricsModule::getHostMetrics()')
    method = src[start:src.index('\n}', start)+2]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name in ('uptime', 'meminfo', 'loadavg'):
            method = method.replace('"/proc/' + name + '"', json.dumps(str(root/name)))
        method = method.replace('std::filesystem::space("/")', 'std::filesystem::space(' + json.dumps(tmp) + ')')
        (root/'donor.cpp').write_text(
            '#include <unistd.h>\n#include <cstring>\n#include <fstream>\n#include <filesystem>\n#include <iostream>\n'
            'struct Host { unsigned uptime_seconds,load1,load5,load15; unsigned long long diskfree1_bytes,freemem_bytes; '
            'char user_string[161]; bool has_user_string; };\n'
            'struct meshtastic_Telemetry {int which_variant; struct {Host host_metrics;} variant;};\n'
            '#define meshtastic_Telemetry_init_zero {}\n#define meshtastic_HostMetrics_init_zero {}\n'
            'constexpr int meshtastic_Telemetry_host_metrics_tag=8; struct {std::string hostMetrics_user_command;} portduino_config;\n'
            'std::string exec(const char *s){return s;}\n'
            'struct HostMetricsModule {static meshtastic_Telemetry getHostMetrics();};\n' + method +
            '\nint main(){portduino_config.hostMetrics_user_command="fixture"; auto h=HostMetricsModule::getHostMetrics().variant.host_metrics;'
            'std::cout<<h.uptime_seconds<<" "<<h.freemem_bytes<<" "<<h.load1<<" "<<h.load5<<" "<<h.load15<<" "<<h.user_string;}\n')
        sys.path.insert(0, str(APP.parent))
        import host_test_toolchain as host
        subprocess.run([*host.cxx(), '-std=c++17', str(root/'donor.cpp'), '-o', str(root/'donor')], check=True)
        (root/'meminfo').write_text('MemTotal: 1000 kB\nMemAvailable: 123 kB\n')
        (root/'uptime').write_text('1704.93 300')
        for loads in ['0.29 12.65 13.01', '0 0.05 0.999', '3.95 2.49 1.92']:
            (root/'loadavg').write_text(loads + ' 1/23 42')
            expected = subprocess.check_output([root/'donor'], text=True).split()
            values = metrics.snapshot(root, 65536, 'fixture')
            fields = ['uptime_seconds', 'freemem_bytes', 'load1', 'load5', 'load15']
            assert [values[k] for k in fields] == [int(v) for v in expected[:5]]
            assert values['user_string'] == expected[5] and values['diskfree1_bytes'] == 65536
            capture = SerialCapture()
            metrics.publish(capture, values, 3)
            packet = capture.packet
            assert packet.channel == 3 and packet.priority == mesh_pb2.MeshPacket.BACKGROUND
            assert packet.decoded.portnum == portnums_pb2.TELEMETRY_APP and not packet.decoded.want_response
            received = telemetry_pb2.Telemetry.FromString(packet.decoded.payload)
            assert received.WhichOneof('variant') == 'host_metrics'
            assert all(getattr(received.host_metrics, key) == v for key, v in values.items())
        assert len(metrics.snapshot(root, 0, 'x'*200)['user_string']) == 160
        assert metrics.snapshot(root, 0, b'example\0suffix')['user_string'] == 'example'
        assert metrics.snapshot(root, 0, b'abc\xff')['user_string'] == 'abc'
        assert 'user_string' not in metrics.snapshot(root, 0, b'\xff\xff')
        assert metrics.snapshot(root/'absent', 0) == {'diskfree1_bytes': 0}
    print('PASS stock Linux metrics, serialized host reports, missing data and label bounds')


if __name__ == '__main__':
    main()
