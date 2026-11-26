#!/usr/bin/env python3
"""Compile production policy and independently pinned Meshtastic donor handlers."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

PIN = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
BASE = Path(__file__).resolve().parents[3]
SRC = BASE / 'zephcore/bench/upstream-differential/source'
TEST = Path(__file__).resolve().parent


def method(source, start):
    # Donor definitions start and end in column zero. Nested blocks are indented.
    a = source.index(start)
    return source[a:source.index('\n}', source.index('\n{', a)) + 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', required=True, type=Path)
    args = parser.parse_args()

    def donor(path):
        return subprocess.check_output(['git', '-C', args.stock, 'show', f'{PIN}:src/{path}'], text=True)

    with tempfile.TemporaryDirectory(prefix='zt-stock-service-') as tmp:
        build = Path(tmp)
        proto = build / 'meshtastic'
        proto.mkdir()
        for f in SRC.glob('*.pb.h'):
            (proto / f.name).symlink_to(f)
        mqtt = method(donor('mqtt/MQTT.cpp'), 'void MQTT::onSend(')
        # Capture the chosen packet at the interface to the envelope/transport.
        mqtt = mqtt[:mqtt.index('    // Generate node ID')] + '    published = p;\n}'
        store = donor('modules/StoreForwardModule.cpp')
        constructor = method(store, 'StoreForwardModule::StoreForwardModule()')
        start = constructor.index('config.device.role ==')
        end = constructor.index(')) {', start)
        (build / 'store_role.inc').write_text('return '+constructor[start:end]+';\n')
        client = method(store, 'bool StoreForwardModule::handleReceivedProtobuf(')
        start = client.index('    case meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS:')
        end = client.index('    default:', start)
        (build / 'store_client.inc').write_text(client[start:end])
        gpio = donor('modules/RemoteHardwareModule.cpp')
        methods = [mqtt, method(store, 'void StoreForwardModule::historySend('),
                   method(store, 'void StoreForwardModule::statsSend('),
                   method(store, 'uint32_t StoreForwardModule::getNumAvailablePackets('),
                   method(store, 'int32_t StoreForwardModule::runOnce('),
                   method(gpio, 'bool RemoteHardwareModule::handleReceivedProtobuf('),
                   method(gpio, 'int32_t RemoteHardwareModule::runOnce(')]
        geo = donor('gps/GeoCoord.cpp')
        methods.extend([method(geo, 'static double cosLatitudeApprox('),
                        method(geo, 'float GeoCoord::latLongToMeter(')])
        for name in ('AirQuality', 'Health'):
            methods.append(method(donor(f'modules/Telemetry/{name}Telemetry.cpp'),
                                  f'bool {name}TelemetryModule::get{name}Telemetry('))
        # Compile the actual two stock periodic mesh decisions; packet delivery and
        # sensor drivers are replaced by clock/airtime captures in this oracle.
        health = donor('modules/Telemetry/HealthTelemetry.cpp')
        health_start = health.index('if (((lastTelemetry == 0)')
        health_test = health[health_start + 4:health.index(') {', health_start)]
        air = donor('modules/Telemetry/AirQualityTelemetry.cpp')
        allowed = air[air.index('bool telemetryAllowed ='):air.index(';',air.index('bool telemetryAllowed ='))+1]
        due_start = air.index('bool telemetryDue = (lastTelemetry == 0) || !Throttle::')
        due = air[due_start:air.index(';',due_start)+1]
        (build/'periodic.inc').write_text('bool stockHealth(uint32_t lastTelemetry){return '+health_test+';}\n'
            +'bool stockAir(uint32_t lastTelemetry,uint32_t telemetryIntervalMs){'
            +allowed+due+'return telemetryDue && telemetryAllowed;}\n')
        main_cpp = (BASE/'mt_only/src/main.cpp').read_text()
        usb = main_cpp[main_cpp.index('  bool sendUsb('):]
        (build/'usb_send.inc').write_text(usb[:usb.index('\n  }')+4])
        serial = main_cpp[main_cpp.index('class SerialHost final:public ClientHost {'):]
        serial = serial[:serial.index('\n};')+3]
        (build/'serial_send.inc').write_text(serial)
        (build / 'oracle.inc').write_text('\n'.join(methods))
        for short in (False, True):
            cxx = shlex.split(os.environ.get('CXX', 'c++'))
            exe = build / 'service-module-check'
            flags = ['-fshort-enums'] if short else []
            objects = []
            for name in ('pb_common','pb_decode','pb_encode'):
                obj = build / (name+'.o')
                subprocess.run([*shlex.split(os.environ.get('CC','cc')), '-O1', '-fsanitize=address,undefined',
                                *flags, '-c', SRC/'nanopb'/(name+'.c'), '-o', obj], check=True)
                objects.append(obj)
            subprocess.run([*cxx, '-std=c++17', '-O1', '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', *(['-fshort-enums'] if short else []),
                            '-I', build, '-I', SRC / 'nanopb', '-I', BASE / 'mt_only/src',
                            '-I', TEST/'sensors', '-I', BASE/'zephcore/adapters/bench/vendor',
                            '-I', BASE/'zephcore/lib/monocypher', BASE/'mt_only/src/Protobuf.cpp',
                            BASE/'mt_only/src/telemetry/CompletionTelemetry.cpp', *objects,
                            TEST/'telemetry_wire.cpp', TEST/'sensor_schedule.cpp', TEST/'sensor_transports.cpp', TEST/'cases.cpp', '-o', exe], check=True)
            subprocess.run([exe], check=True, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0',
                                                   'UBSAN_OPTIONS':'halt_on_error=1'})

if __name__ == '__main__':
    main()
