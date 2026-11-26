#!/usr/bin/env python3
"""Compare the documented module owners with the pinned Meshtastic registration and source tree."""
import argparse
from pathlib import Path
import re
import subprocess

PIN = '8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c'
APP = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--version', choices=('2.8.1','2.8.2'), default='2.8.1')
    args=parser.parse_args()
    stock=args.stock
    candidate=args.version=='2.8.2'
    pin='d5f16191ca28b01b5024f590f04a67f25d66f1b1' if candidate else PIN
    def git(*args):
        return subprocess.check_output(['git', '-C', str(stock), *args], text=True)
    source = git('show', pin + ':src/modules/Modules.cpp')
    uncommented = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    registered = set(re.findall(r'\bnew\s+(\w+)\s*\(', uncommented))
    files = git('ls-tree', '-r', '--name-only', pin, 'src/modules').splitlines()
    units = {Path(p).stem for p in files if p.endswith('.cpp') and '/Sensor/' not in p} - {'Modules'}
    sensor_units = [p for p in files if p.endswith('.cpp') and '/Sensor/' in p]
    sensors = {Path(path).stem for path in sensor_units}
    sensor_doc = (APP / 'docs/UPSTREAM-TELEMETRY-SENSORS.md').read_text()
    labels = re.findall(r'^\| ([A-Za-z]\w*(?:, [A-Za-z]\w*)*) \|', sensor_doc, re.M)
    listed = [name for group in labels for name in group.split(', ')]
    doc = (APP / 'docs/UPSTREAM-MODULE-INVENTORY.md').read_text()
    rows = re.findall(r'^\| `(\w+)` \| (registered|companion|support) \| `([^\x60]+)` \| `([^\x60]+)` \|', doc, re.M)
    names = {name for name, *_ in rows}
    registrations = {name for name, kind, _, _ in rows if kind == 'registered'}
    covered_units = {unit for _, _, unit, _ in rows}
    missing_files = {owner for _, _, _, owner in rows if not (APP / owner).is_file()}
    if candidate:
        # Stock compile guards exclude these owners from the normal S3 and T096 profiles.
        new_registrations={'DMShellModule','DMShellTestModule'}
        new_units={'DMShell','DMShellTestModule'}
        new_sensors={'RAKSensorHub','RAKSensorHubUplink'}
        profile_doc=(APP/'docs/MESHTASTIC-2.8.2-DELTA.md').read_text()
        if any('`'+name+'`' not in profile_doc for name in new_registrations|new_units|new_sensors):
            raise SystemExit('missing applicability entry')
        registrations |= new_registrations
        covered_units |= new_units
        listed.extend(sorted(new_sensors))
    errors = {'duplicate rows':len(rows) != len(names), 'missing registrations':registered-registrations,
              'unexpected registrations':registrations-registered, 'missing translation units':units-covered_units,
              'missing local files':missing_files,
              'undocumented sensor units':sensors-set(listed), 'unexpected sensor units':set(listed)-sensors,
              'repeated sensor units':{name for name in listed if listed.count(name)>1}}
    for field, value in errors.items():
        if value:
            raise SystemExit(f'{field}: {value}')
    print(f'{len(registered)} upstream registrations, {len(units)} module translation units, '
          f'{len(sensor_units)} sensor backend translation units, {len(rows)} documented entries')


if __name__ == '__main__':
    main()
