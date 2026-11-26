#!/usr/bin/env python3
"""Meshtastic 8e6a88d HostMetricsModule for a Linux host attached to ZT by USB."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import time

UINT32_MAX = (1 << 32) - 1

def single(value):
    return struct.unpack('=f', struct.pack('=f', value))[0]


def snapshot(proc, disk_free, label=''):
    result = {'diskfree1_bytes': disk_free}
    try:
        result['uptime_seconds'] = min(UINT32_MAX, max(0, int((proc / 'uptime').read_text().split()[0].split('.')[0])))
    except (OSError, ValueError, IndexError):
        pass
    try:
        for line in (proc / 'meminfo').read_text().splitlines():
            fields = line.split()
            if fields and fields[0] == 'MemAvailable:' and len(fields) == 3 and fields[2] == 'kB':
                result['freemem_bytes'] = max(0, int(fields[1])) * 1024
                break
    except (OSError, ValueError):
        pass
    try:
        loads = (proc / 'loadavg').read_text().split()
        for name, value in zip(('load1', 'load5', 'load15'), loads[:3]):
            result[name] = min(UINT32_MAX, max(0, int(single(single(float(value)) * 100))))
    except (OSError, ValueError, OverflowError):
        pass
    raw = label.encode() if isinstance(label, str) else label
    if len(raw) > 1:
        # The donor writes a terminated C string into a 161-byte nanopb field.
        text = raw.split(b'\0', 1)[0][:160].decode(errors='ignore')
        if text:
            result['user_string'] = text
    return result

def publish(interface, report, channel):
    from meshtastic.protobuf import mesh_pb2, portnums_pb2, telemetry_pb2
    telemetry = telemetry_pb2.Telemetry()
    telemetry.host_metrics.SetInParent()
    for name, value in report.items():
        setattr(telemetry.host_metrics, name, value)
    interface.sendData(telemetry, destinationId='^all', portNum=portnums_pb2.TELEMETRY_APP,
                       wantAck=False, wantResponse=False, channelIndex=channel,
                       priority=mesh_pb2.MeshPacket.Priority.BACKGROUND)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True, help='Local USB serial device')
    parser.add_argument('--interval-minutes', type=int, default=0, help='Zero disables reports')
    parser.add_argument('--channel', type=int, choices=range(8), default=0)
    parser.add_argument('--label-command', nargs=argparse.REMAINDER, help='Local argv producing the optional host label')
    args = parser.parse_args()
    if args.interval_minutes < 0:
        parser.error('interval must be nonnegative')
    if not args.interval_minutes:
        return
    from meshtastic.serial_interface import SerialInterface
    with SerialInterface(devPath=args.device) as interface:
        while True:
            label = ''
            if args.label_command:
                try:
                    label = subprocess.check_output(args.label_command, timeout=10)
                except (OSError, subprocess.SubprocessError):
                    pass
            fs = os.statvfs('/')
            publish(interface, snapshot(Path('/proc'), fs.f_bavail * fs.f_frsize, label), args.channel)
            time.sleep(args.interval_minutes * 60)

if __name__ == '__main__':
    main()
