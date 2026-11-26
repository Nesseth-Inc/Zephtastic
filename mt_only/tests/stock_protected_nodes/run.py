#!/usr/bin/env python3
"""Compare XIAO protected capacity and exercise the pre-migration reader on saved records."""
import argparse
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

here=Path(__file__).resolve().parent
app=here.parents[1]
root=app.parent
sys.path.insert(0,str(root))
import host_test_toolchain as host
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--stock',required=True,type=Path)
a=p.parse_args()
def show(repo,pin,path):
    return subprocess.check_output(['git','-C',repo,'show',pin+':'+path])
donor=show(a.stock,'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','src/mesh/NodeDB.cpp').decode()
header=show(a.stock,'8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c','src/mesh/NodeDB.h').decode()
helpers=''.join(line+'\n' for line in header.splitlines() if line.startswith('#define NODEINFO_BITFIELD_'))
for name in ('nodeInfoLiteIsFavorite','nodeInfoLiteIsIgnored','nodeInfoLiteIsKeyManuallyVerified','nodeInfoLiteIsProtected','nodeInfoLiteSetBit'):
    helpers+=re.search(r'inline (?:bool|void) '+name+r'\([^\n]+\)\n\{.*?\n\}',header,re.S).group(0)+'\n'
oracle=donor[donor.index('int NodeDB::numProtectedNodes() const'):donor.index('bool NodeDB::set_favorite(')]
with tempfile.TemporaryDirectory(prefix='zt-protected-nodes-') as tmp:
    out=Path(tmp)
    (out/'oracle.inc').write_text(oracle)
    (out/'helpers.inc').write_text(helpers)
    legacy=out/'old';legacy.mkdir()
    for h in ('NodeJournal.h','NodeCache.h'):
        (legacy/h).write_bytes(show(root,'a0844347c45d1eb85745b49beba7634b29e0fab5','mt_only/src/'+h))
    for abi in ([],['-fshort-enums']):
        common=[*host.cxx(),'-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*abi]
        for case,extra in [('oracle',['-I'+str(out)]),('cases',['-DCONFIG_MT_XIAO_LARGE_NODEDB=1']),('cases',['-I'+str(legacy)])]:
            subprocess.run([*common,*extra,'-I'+str(app/'src'),'-I'+str(root/'zephcore/adapters/bench/vendor'),here/(case+'.cpp'),'-o',out/'test'],check=True)
            subprocess.run([out/'test',out],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
