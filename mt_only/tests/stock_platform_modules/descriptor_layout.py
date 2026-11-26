#!/usr/bin/env python3
"""Prove compact and four-word nanopb descriptors expose identical fields on each ABI."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

APP = Path(__file__).resolve().parents[2]
ROOT = APP.parent
sys.path.insert(0,str(ROOT))
import host_test_toolchain as host

source = (APP/'src/Protobuf.cpp').read_text()
pattern = r'PB_BIND\(\s*(\w+),\s*\w+,\s*(?:AUTO|[1248])\s*\)'
names = re.findall(pattern, source)
assert names and len(names) == len(set(names))
proto = ROOT/'zephcore/bench/upstream-differential/source'
with tempfile.TemporaryDirectory() as temp:
    out = Path(temp)
    (out/'meshtastic').mkdir()
    for f in proto.glob('*.pb.h'):
        (out/'meshtastic'/f.name).symlink_to(f)
    fixture = '''
#include <cstdio>
#include <cstdint>
#include <pb_common.h>
struct Named {const char *name; const pb_msgdesc_t *descriptor;};
static Named all[]={
''' + ','.join('{"'+name+'",&'+name+'_msg}' for name in names) + '''
};
template<class Message> void describe(const Named &entry) {
    Message value{};
    auto offset=[&](const void *p)->long long{
        return p?reinterpret_cast<const char *>(p)-reinterpret_cast<const char *>(&value):-1;
    };
    auto *d=entry.descriptor;
    printf("%s %u %u %u\\n",entry.name,unsigned(d->field_count),unsigned(d->required_field_count),unsigned(d->largest_tag));
    pb_field_iter_t it;
    if(pb_field_iter_begin(&it,d,&value))do{
        const char *sub="-";
        if(it.submsg_desc){
            sub="?";
            for(auto &other:all)if(other.descriptor==it.submsg_desc){sub=other.name;break;}
        }
        printf("%u %u %u %u %lld %lld %lld %s\\n",unsigned(it.tag),unsigned(it.type),
               unsigned(it.data_size),unsigned(it.array_size),offset(it.pField),offset(it.pData),it.pSize==&it.array_size?-2:offset(it.pSize),sub);
    }while(pb_field_iter_next(&it));
}
int main(){
''' + '\n'.join('describe<'+name+'>(all['+str(i)+']);' for i,name in enumerate(names)) + '\n}\n'
    for abi in ([], ['-fshort-enums']):
        flags = [*abi, '-DCONFIG_MT_GPIO_MODULES=1', '-DCONFIG_MT_POWER_STRESS=1', '-I'+str(out), '-I'+str(proto/'nanopb'), '-I'+str(APP/'src')]
        common = out/'common.o'
        subprocess.run([*host.cc(),*flags,'-c',proto/'nanopb/pb_common.c','-o',common],check=True)
        outputs=[]
        for name,code in [('t096',source),('compact',source),('wide',re.sub(pattern,lambda m:'PB_BIND('+m[1]+', '+m[1]+', 4)',source))]:
            path=out/(name+'.cpp')
            path.write_text(code+fixture)
            exe=out/name
            target = ['-DCONFIG_MT_PKI_FRESH_DOMAIN_ONLY=1'] if name == 't096' else []
            subprocess.run([*host.cxx(),'-std=c++17',*flags,*target,path,common,'-o',exe],check=True)
            outputs.append(subprocess.check_output([exe]))
        assert outputs[0] == outputs[1] == outputs[2], 'nanopb field interpretation differs'
        print(f'PASS {len(names)} messages / {len(outputs[0].splitlines())-len(names)} fields; {abi or "default ABI"}')
