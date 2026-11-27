#!/usr/bin/env python3
"""Compare report writers, local phone delivery and the Linux host producer against 2.8.2."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
ROOT = APP.parent
REVISION = 'd5f16191ca28b01b5024f590f04a67f25d66f1b1'
SHARED = APP / 'tests/stock_service_modules'
sys.path.insert(0, str(ROOT))
import host_test_toolchain as host


def method(unit, signature):
    start = unit.index(signature)
    return unit[start:unit.index('\n}', start) + 2]


def prepare(stock, out):
    def source(path):
        return subprocess.check_output(['git', '-C', stock, 'show', f'{REVISION}:src/modules/{path}'], text=True)

    def telemetry(name):
        return source('Telemetry/' + name)

    conversions = telemetry('UnitConversions.h').split('class UnitConversions', 1)[1]
    conversions_cpp = telemetry('UnitConversions.cpp').split('#include "UnitConversions.h"', 1)[1]
    air = telemetry('AirQualityTelemetry.cpp')
    health = telemetry('HealthTelemetry.cpp')
    power = telemetry('PowerTelemetry.cpp')
    (out / 'reports.inc').write_text('\n'.join((
        method(air, 'bool AirQualityTelemetryModule::getAirQualityTelemetry('),
        method(health, 'bool HealthTelemetryModule::getHealthTelemetry('),
        method(power, 'bool PowerTelemetryModule::getPowerTelemetry('),
        *(method(telemetry('Sensor/' + name + '.cpp'), 'bool ' + name + '::getPowerMetrics(')
          for name in ('INA226Sensor', 'INA3221Sensor')),
        'class UnitConversions' + conversions, conversions_cpp,
    )))
    # DeviceTelemetry's constructor registers an OSThread deadline from this
    # stock initializer. Drive its returned intervals separately from ZT.
    def mesh_source(name):
        return subprocess.check_output(['git', '-C', stock, 'show', f'{REVISION}:src/mesh/{name}'], text=True)
    mesh_header = mesh_source('MeshModule.h')
    constants = [line for line in mesh_header.splitlines() if line.startswith(
        ('#define MESHMODULE_MIN_BROADCAST_DELAY_MS ', '#define MESHMODULE_BROADCAST_SPACING_MS '))]
    if len(constants) != 2 or 'setIntervalFromNow(setStartDelay());' not in telemetry('DeviceTelemetry.h'):
        raise ValueError('Unrecognized stock DeviceTelemetry startup')
    # Count calls in the embedded registration order. Thread init callbacks
    # run later. Portduino's optional HostMetrics is absent from this profile.
    registrations = ('NodeInfoModule', 'MeshBeaconBroadcastModule', 'PositionModule',
                     'KeyVerificationModule', 'DeviceTelemetryModule')
    module_list = source('Modules.cpp')
    offsets = [module_list.index('new ' + name + '()') for name in registrations]
    if offsets != sorted(offsets):
        raise ValueError('Embedded periodic registration order changed')
    constructors = ('NodeInfoModule.cpp', 'MeshBeaconModule.cpp', 'PositionModule.cpp', 'KeyVerificationModule.h')
    for path in constructors:
        if 'setIntervalFromNow(setStartDelay());' not in source(path):
            raise ValueError(f'Missing periodic constructor registration in {path}')
    (out / 'start.inc').write_text('\n'.join([*constants,
        method(mesh_source('MeshModule.cpp'), 'int32_t MeshModule::setStartDelay()')]))
    position = source('PositionModule.cpp')
    (out / 'phone-position.inc').write_text('\n'.join((
        method(position, 'bool PositionModule::sendOurPositionToPhone('),
        method(position, 'bool PositionModule::shouldSendPositionToPhone('),
    )))
    (out / 'phone-device.inc').write_text(method(telemetry('DeviceTelemetry.cpp'), 'int32_t DeviceTelemetryModule::runOnce('))

    # Compile the scheduling predicates inside the existing clock/airtime fixture.
    start = health.index('if (((lastTelemetry == 0)')
    health_test = health[start + 4:health.index(') {', start)]
    allowed = air.index('bool telemetryAllowed =')
    due = air.index('bool telemetryDue = (lastTelemetry == 0) || !Throttle::')
    (out / 'periodic.inc').write_text(
        'bool stockHealth(uint32_t lastTelemetry){return ' + health_test + ';}\n' +
        'bool stockAir(uint32_t lastTelemetry,uint32_t telemetryIntervalMs){' +
        air[allowed:air.index(';', allowed) + 1] + air[due:air.index(';', due) + 1] +
        'return telemetryDue && telemetryAllowed;}\n')

    runtime = (APP / 'src/main.cpp').read_text()
    start = runtime.index('  bool sendUsb(')
    (out / 'usb_send.inc').write_text(runtime[start:runtime.index('\n  }', start) + 4])
    start = runtime.index('class SerialHost final:public ClientHost {')
    (out / 'serial_send.inc').write_text(runtime[start:runtime.index('\n};', start) + 3])
    fanout = method(runtime, 'static uint8_t publishModuleData(const uint8_t *data,size_t size,uint32_t id,unsigned channel,bool idleOnly,uint8_t recipients,bool background){')
    (out / 'fanout.inc').write_text(fanout.removeprefix('static '))


def profile(out, abi):
    proto = ROOT / 'zephcore/bench/upstream-differential/source'
    vendor = ROOT / 'zephcore/adapters/bench/vendor'
    mono = ROOT / 'zephcore/lib/monocypher'
    paths = (out, APP/'src', APP/'tests', APP/'src/pki', APP/'experimental/pki',
             SHARED/'sensors', proto/'nanopb', vendor, mono)
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *abi,
             *['-I'+str(p) for p in paths], *host.crypto_compile_flags()]
    objects = []
    for src in [mono/'monocypher.c', mono/'monocypher-ed25519.c',
                *(proto/'nanopb'/n for n in ('pb_common.c','pb_decode.c','pb_encode.c'))]:
        obj = out/(src.stem+'.o')
        subprocess.run([*host.cc(), '-std=c11', *flags, '-c', src, '-o', obj], check=True)
        objects.append(obj)
    exe = out/'reports'
    subprocess.run([*host.cxx(), '-std=c++17', *flags, HERE/'reports.cpp', HERE/'phone.cpp',
                    APP/'src/telemetry/CompletionTelemetry.cpp', SHARED/'telemetry_wire.cpp',
                    SHARED/'sensor_schedule.cpp', SHARED/'sensor_transports.cpp', APP/'src/Protobuf.cpp',
                    vendor/'MtCodec.cpp', vendor/'MtSignature.cpp', *objects, *host.crypto_link_flags(),
                    '-o', exe], check=True)
    return json.loads(subprocess.check_output([exe], text=True, env={**os.environ,
                          'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='zt-reports-') as directory:
        out = Path(directory)
        (out/'meshtastic').symlink_to(ROOT/'zephcore/bench/upstream-differential/source', target_is_directory=True)
        prepare(args.stock, out)
        profiles = {label: profile(out, flags) for label, flags in [('default', []), ('short-enums', ['-fshort-enums'])]}
    subprocess.run([sys.executable, APP/'tests/stock_platform_modules/host_metrics.py', '--stock', args.stock,
                    '--stock-revision', REVISION], check=True)
    def row(evidence):
        return {'kind': 'differential', 'evidence': evidence}
    receipt = {
        'stock_revision': REVISION, 'profiles': profiles,
        'modules': {
            'AirQualityTelemetryModule': row('Partial, missing and combined particulate/CO2 inputs matched the stock aggregator; modeled Zephyr channels also exercised the request, cache and stale paths.'),
            'HealthTelemetryModule': row('Optical and temperature availability, failure combinations and successful wire payloads matched the stock reader on both enum ABIs.'),
            'PowerTelemetryModule': row('Compiled INA226/INA3221 and PowerTelemetry donor methods matched solicited and announced payloads for absent, individual and combined measured devices.'),
            'HostMetricsModule': row('The Linux producer matched the stock proc-fixture uptime, memory, load and label fields and serialized the report using the Meshtastic client on the selected channel.'),
            'UnitConversions': row('The three arithmetic conversion bodies and display clamp matched on finite, extreme, signed-zero and nonfinite inputs on both enum ABIs.'),
        },
        'local_phone': {
            'position': 'Exact fixed or fresh self-position, 60-second success-based cadence, busy queues, wraparound, configuration-only sessions and owner-specific denial checked with two phone transports and two excluded owners.',
            'device': 'From first registration, ordinary/tracker checks start at 90/75 seconds, then every minute. Stock runOnce and production choose RF or idle phone delivery, including background-priority LocalStats at 15 minutes and per-session pressure.',
            'limits': 'Full resolution is sent only on completed normal USB or encrypted BLE sessions. Reconnects preserve the per-owner timer. Serial/TCP/browser, RF and MQTT do not receive this unsolicited full-resolution packet.',
        },
        'hardware': False,
        'limits': ['Sensors and transport queues use deterministic inputs. No physical accuracy, BLE load, GNSS acquisition, sleep/wake timing or RF operation is measured.',
                   'PowerReport retains valid observations from other sensors when one fails; the stock PowerTelemetry aggregate can suppress the report if the last present driver returns false.',
                   'The optional Linux bridge broadcasts host metrics only when enabled; the local phone position and idle telemetry routes do not use that bridge.',
                   'These startup registrations model the embedded NodeInfo, Beacon, Position and KeyVerification profile. Portduino or stock builds that exclude a module can use other slots. Environment, power, health and air retain their native acquisition and retry timelines.'],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
