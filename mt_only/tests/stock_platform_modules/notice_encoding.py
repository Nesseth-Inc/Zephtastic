#!/usr/bin/env python3
"""Compare every packed T096 companion notification with the readable source, including truncated destinations."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

app=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(app.parent))
import host_test_toolchain as host
spec=importlib.util.spec_from_file_location('packer',app/'scripts/pack-notices.py')
packer=importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)
source=app/'src/Companion.h'
raw=packer.messages(source)
with tempfile.TemporaryDirectory() as tmp:
    work=Path(tmp)
    (work/'mt_notice_table.h').write_text(packer.generate(source))
    text=['#include "companion/NoticeText.h"','#include <cassert>','#include <cstring>',
          'int main(){struct Case{const char *original,*packed;};Case cases[]={']
    for message in raw:
        literal=json.dumps(message.decode('ascii'))
        text.append('{'+literal+',MT_NOTICE('+literal+')},')
    text += ['{"Unpacked fallback","Unpacked fallback"}};',
             'for(auto item:cases)for(size_t n=0;n<=strlen(item.original)+2;++n){',
             'char dst[512];memset(dst,0x55,sizeof(dst));mt::companion::notice::copy(dst,n,item.packed);',
             'assert(dst[n]==0x55);if(n){size_t len=strlen(item.original);if(len>=n)len=n-1;',
             'assert(strlen(dst)==len && !memcmp(dst,item.original,len));}}}']
    (work/'verify.cpp').write_text('\n'.join(text))
    subprocess.run([*host.cxx(),'-DMT_TEST_COMPACT_NOTICES=1','-std=c++17','-I'+str(work),
                    '-I'+str(app/'src'),work/'verify.cpp','-o',work/'verify'],check=True)
    subprocess.run([work/'verify'],check=True)
print(f'PASS {len(raw)} notices, all truncation lengths and plain-text fallback')
