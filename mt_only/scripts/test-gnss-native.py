#!/usr/bin/env python3
"""Actual GNSS parser/matcher; synthetic UART/publication boundary, no hardware."""
import argparse,hashlib,json,os,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
app=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(exist_ok=False)
test=app/'tests/gnss_native';sdk=test/'sdk';overlay=app/'dependency-overlay/zephyr/drivers/gnss'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
provenance=json.loads((test/'PROVENANCE.json').read_text())
assert provenance['SDK_own_git_identity_claimed'] is False and provenance['kernel_runtime_modeled'] is True
assert set(provenance['files'])=={str(p.relative_to(sdk))for p in sdk.rglob('*')if p.is_file()}
for name,row in provenance['files'].items():
 assert (sdk/name).stat().st_size==row['bytes'] and sha(sdk/name)==row['sha256']
stub=out/'stubs';(stub/'zephyr/drivers/gnss').mkdir(parents=True);(stub/'zephyr/modem').mkdir()
(stub/'zephyr/data').mkdir()
(stub/'zephyr/kernel.h').write_text('''#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define __ASSERT(c,...) assert(c)
#define BIT(x) (1u<<(x))
#define ARG_UNUSED(x) (void)(x)
#define snprintk snprintf
#define vsnprintk vsnprintf
''')
(stub/'zephyr/types.h').write_text('#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n')
(stub/'zephyr/device.h').write_text('#pragma once\nstruct device {int unused;};\n')
(stub/'zephyr/modem/chat.h').write_text('#pragma once\nstruct modem_chat {int unused;};\n')
header=(sdk/'include/zephyr/drivers/gnss.h').read_text()
# Exact SDK value declarations only. Kernel/syscall/driver runtime is modeled,
# not claimed as compiled native Zephyr. Real target build is separate proof.
names=['enum gnss_system','enum gnss_fix_status','enum gnss_fix_quality','struct gnss_info','struct gnss_time','struct gnss_data','struct gnss_satellite']
parts=[]
for name in names:
 m=re.search(re.escape(name)+r'\s*\{.*?\n\};',header,re.S);assert m,name;parts.append(m.group())
(stub/'zephyr/drivers/gnss.h').write_text('#pragma once\n#include <zephyr/kernel.h>\n#include <zephyr/device.h>\n#include <zephyr/data/navigation.h>\n'+ '\n'.join(parts)+'\n')
shutil.copyfile(sdk/'include/zephyr/data/navigation.h',stub/'zephyr/data/navigation.h')
(stub/'zephyr/drivers/gnss/gnss_publish.h').write_text('#pragma once\n#include <zephyr/drivers/gnss.h>\nvoid gnss_publish_data(const struct device *,const struct gnss_data *);\n')
sources=[test/'cases.c',overlay/'gnss_nmea0183.c',overlay/'gnss_nmea0183_match.c',sdk/'drivers/gnss/gnss_parse.c']
pins={str(p):sha(p)for p in [Path(__file__).resolve(),test/'PROVENANCE.json',*sources,*sdk.rglob('*')]if p.is_file()};cases=[]
for short in [False,True]:
 d=out/('short'if short else'default');d.mkdir();exe=d/'cases'
 cmd=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-Wno-pointer-sign','-fsanitize=address,undefined','-fno-omit-frame-pointer',*(['-fshort-enums']if short else[]),'-I'+str(stub),'-I'+str(sdk/'drivers/gnss'),*map(str,sources),'-o',str(exe)]
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(d/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(d/'run.log').write_text(r.stdout+r.stderr);r.check_returncode()
 cases.append({'short_enums':short,'command':cmd,'returncode':r.returncode,'pass_lines':[x for x in r.stdout.splitlines()if x.startswith('PASS ')]})
assert all(sha(Path(n))==h for n,h in pins.items())
(out/'RESULT.json').write_text(json.dumps({'status':'PASS','hardware':False,'real_UART':False,'kernel_driver_runtime_modeled':True,'actual_parser_and_matcher':True,'ASAN_UBSAN':True,'source_pins':pins,'cases':cases},indent=2,sort_keys=True)+'\n')
print('PASS native GNSS parser/matcher adversaries in both enum modes; synthetic I/O only')
