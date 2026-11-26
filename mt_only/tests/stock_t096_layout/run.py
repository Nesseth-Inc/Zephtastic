#!/usr/bin/env python3
"""Check T096 offline first-install refusals, physical partition ranges and boot-time reset-floor reads."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
APP=HERE.parents[1]
CHECK=APP/'scripts/check-t096-fresh-layout.py'
FLASH=0x100000
FULL={'code_partition':(0x26000,0xb6000),'storage_partition':(0xdc000,0x4000),
      'lfs_partition':(0xe0000,0x13000),'mt_reset_floor':(0xf3000,0x1000),'uf2_partition':(0xf4000,0xc000)}
PAGED={**FULL,'code_partition':(0x26000,0xb7000),'storage_partition':(0xdd000,0x4000),'lfs_partition':(0xe1000,0x12000)}
ORIGINAL={'code_partition':(0x26000,0xaa000),'storage_partition':(0xd0000,0x4000),
          'lfs_partition':(0xd4000,0x1f000),'mt_reset_floor':(0xf3000,0x1000),'uf2_partition':(0xf4000,0xc000)}

def enrollment(tmp):
    image=bytearray(b'\xff'*FLASH)
    path=tmp/'synthetic.bin'
    def check(flags,code,word):
        path.write_bytes(image)
        cmd=[sys.executable,'-B',str(CHECK),str(path),*flags]
        result=subprocess.run(cmd,text=True,capture_output=True)
        assert result.returncode==code and word in (result.stdout+result.stderr),(flags,code,result)
        assert path.read_bytes()==image
    check(['--full-companion'],2,'UF2 boot reservation is blank')
    image[-1]=0xa5
    for flags in ([],['--device-reset'],['--full-companion'],['--device-reset','--full-companion']):
        check(flags,0,'erased')
    count=0
    regions=(('legacy PKI history',0xa6000,0xd0000),('settings NVS',0xd0000,0xd4000),
             ('original LittleFS volume',0xd4000,0xf4000))
    full=['--full-companion']
    for expected,start,end in regions:
        # Endpoints of all previous erase pages include the new application
        # end, relocated NVS, private history start and independent floor.
        for page in range(start,end,4096):
            for at in (page,page+4095):
                image[at]=0x7f
                check(full,1,expected);count+=1
                image[at]=0xff
    for at,expected in ((0xa6000,'legacy PKI history'),(0xd3fff,'settings NVS'),(0xf3fff,'original LittleFS volume')):
        image[at]=0
        check(['--device-reset'],1,expected)
        check([],0 if at>=0xd4000 else 1,'erased' if at>=0xd4000 else expected)
        image[at]=255
    image[0xa5fff]=image[0xf4000]=0
    check(full,0,'erased')
    del image[-1];check(full,2,'exact 1 MiB');image.extend(b'\xa5\xa5')
    check(full,2,'exact 1 MiB')
    print(f'PASS offline checker rejects {count} occupied page boundaries, short/long images and absent UF2; ordinary fresh profile retains its original enrollment scope')

def floor(tmp):
    mocks=tmp/'include/zephyr'
    for rel in ('drivers','settings','storage'):(mocks/rel).mkdir(parents=True)
    (mocks/'devicetree.h').write_text('#define DT_NODE_EXISTS(x) (x)\n#define DT_NODELABEL(x) x\n#define mt_reset_floor 1\n')
    (mocks/'drivers/flash.h').write_text(
        '#pragma once\n#include <stddef.h>\nstruct device{};const device *flashDevice();bool device_is_ready(const device *);\n'
        'int flash_read(const device *,unsigned,void *,size_t);size_t flash_get_write_block_size(const device *);\n'
        'int flash_write(const device *,unsigned,const void *,size_t);\n')
    (mocks/'storage/flash_map.h').write_text(
        '#define FIXED_PARTITION_DEVICE(n) flashDevice()\n#define FIXED_PARTITION_OFFSET(n) OFFSET_##n\n'
        '#define FIXED_PARTITION_SIZE(n) SIZE_##n\n#define OFFSET_mt_reset_floor 0xf3000\n#define SIZE_mt_reset_floor 0x1000\n'
        '#if defined(CONFIG_MT_T096_PAGED_LAYOUT)\n#define OFFSET_lfs_partition 0xe1000\n#define SIZE_lfs_partition 0x12000\n'
        '#elif defined(CONFIG_MT_T096_FULL_COMPANION)\n#define OFFSET_lfs_partition 0xe0000\n#define SIZE_lfs_partition 0x13000\n'
        '#else\n#define OFFSET_lfs_partition 0xd4000\n#define SIZE_lfs_partition 0x1f000\n#endif\n')
    (mocks/'settings/settings.h').write_text(
        '#pragma once\n#include <stddef.h>\n#define SETTINGS_FULL_NAME_LEN 64\nusing settings_read_cb=int (*)(void *,void *,size_t);\n'
        'int settings_load_subtree_direct(const char *,int (*)(const char *,size_t,settings_read_cb,void *,void *),void *);\n')
    cxx=shlex.split(os.environ.get('CXX','c++'))
    for extends in ([],['MT_T096_FULL_COMPANION'],['MT_T096_FULL_COMPANION','MT_T096_PAGED_LAYOUT']):
        cmd=[*cxx,'-std=c++17','-Wall','-Wextra','-Werror','-DCONFIG_BOARD_HELTEC_T096=1','-DCONFIG_MT_PKI_TARGET_RUNTIME=1',
             '-I'+str(mocks.parent),'-I'+str(APP/'src'),str(HERE/'floor.cpp'),'-o',str(tmp/'floor')]
        cmd[len(cxx):len(cxx)]=['-DCONFIG_'+name+'=1' for name in extends]
        subprocess.run(cmd,check=True);subprocess.run([tmp/'floor'],check=True)

def pair(block):
    match=re.search(r'\breg\s*=\s*<\s*(0[xX][\da-fA-F]+|\d+)\s+(0[xX][\da-fA-F]+|\d+)\s*>',block)
    assert match,block
    return tuple(int(value,0) for value in match.groups())

def source_maps():
    # Check the original and optional override as they appear in the repository.
    root=APP.parent
    base=(root/'zephcore/boards/common/nrf52_partitions_sdv6.dtsi').read_text()
    overlays=APP/'boards'
    known={}
    for name in ('code_partition','storage_partition','lfs_partition','uf2_partition'):
        body=re.search(r'\b'+name+r':\s*partition@[\da-fA-F]+\s*\{([^}]+)\}',base).group(1)
        known[name]=pair(body)
    for name in ('t096-fresh-domain.overlay','t096-device-reset.overlay'):
        s=(overlays/name).read_text()
        for node,body in re.findall(r'&(\w+)\s*\{([^{}]+)\}',s):
            if node in known:known[node]=pair(body)
        found=re.search(r'mt_reset_floor:\s*partition@\w+\s*\{([^}]+)\}',s)
        if found:known['mt_reset_floor']=pair(found.group(1))
    assert known==ORIGINAL,known
    for node,body in re.findall(r'&(\w+)\s*\{([^{}]+)\}',(overlays/'t096-full-companion.overlay').read_text()):
        known[node]=pair(body)
    assert known==FULL,known
    assert sum(FULL[n][1] for n in ('code_partition','storage_partition','lfs_partition','mt_reset_floor'))==0xf4000-0x26000
    for node,body in re.findall(r'&(\w+)\s*\{([^{}]+)\}',(overlays/'t096-paged-companion.overlay').read_text()):
        known[node]=pair(body)
    assert known==PAGED,known
    ranges=sorted(PAGED.values())
    assert all(start+size==end for (start,size),(end,_) in zip(ranges,ranges[1:]))
    print('PASS separate 732 KiB layout and 18-sector filesystem')
    print('PASS original 680 KiB layout and independent 728 KiB layout preserve the UF2 reservation and fixed reset floor')

def linked_files(directory,app):
    address,capacity=app
    raw=(directory/'zephyr.bin').read_bytes()
    assert raw and len(raw)<=capacity and address+len(raw)<=address+capacity
    elf=(directory/'zephyr.elf').read_bytes()
    assert elf[:6]==b'\x7fELF\x01\x01' and struct.unpack_from('<H',elf,18)[0]==40
    phoff=struct.unpack_from('<I',elf,28)[0];phsize,count=struct.unpack_from('<HH',elf,42)
    assert phsize==32
    segments=[];linked=bytearray(len(raw));covered=bytearray(len(raw))
    for i in range(count):
        kind,offset,_,paddr,size,_,_,_=struct.unpack_from('<IIIIIIII',elf,phoff+i*phsize)
        if kind!=1 or not size:continue
        assert offset+size<=len(elf) and address<=paddr and paddr+size<=address+capacity,(hex(paddr),size)
        start=paddr-address
        assert start+size<=len(raw) and not any(covered[start:start+size])
        linked[start:start+size]=elf[offset:offset+size]
        covered[start:start+size]=bytes([1])*size
        segments.append(paddr)
    assert segments and min(segments)==address
    uf2=(directory/'zephyr.uf2').read_bytes();assert uf2 and len(uf2)%512==0
    image=bytearray();blocks=len(uf2)//512
    for i in range(blocks):
        at=512*i
        m0,m1,flags,target,length,block,total,family=struct.unpack_from('<IIIIIIII',uf2,at)
        assert (m0,m1)==(0x0a324655,0x9e5d5157) and struct.unpack_from('<I',uf2,at+508)[0]==0x0ab16f30
        assert flags==0x2000 and family==0xada52840 and block==i and total==blocks and length==256
        assert target==address+len(image) and target+length<=address+capacity
        image.extend(uf2[at+32:at+32+length])
    assert len(image)>=len(raw)
    # ELF program headers and objcopy need not agree on alignment fill. Verify
    # each programmed byte against the address-bearing Intel HEX output.
    marked=bytearray(len(raw));upper=0;end=False
    for line in (directory/'zephyr.hex').read_text().splitlines():
        assert line.startswith(':') and not end
        record=bytes.fromhex(line[1:]);assert len(record)==record[0]+5 and sum(record)%256==0
        offset=int.from_bytes(record[1:3],'big');kind=record[3];data=record[4:-1]
        if kind==0:
            at=upper+offset-address;until=at+len(data)
            assert data and 0<=at<until<=len(raw) and not any(marked[at:until])
            assert all(covered[at:until]) and data==raw[at:until]==linked[at:until]==image[at:until]
            marked[at:until]=bytes([1])*len(data)
        elif kind in (2,4):
            assert not offset and len(data)==2
            upper=int.from_bytes(data,'big')<<(4 if kind==2 else 16)
        elif kind in (3,5):assert not offset and len(data)==4
        elif kind==1:assert not data and not offset;end=True
        else:raise ValueError(kind)
    assert end and marked[0] and marked[-1]
    assert all(raw[i] in (0,255) and image[i] in (0,255) for i,filled in enumerate(marked) if not filled)
    tail=image[len(raw):];assert not tail or set(tail) in ({0},{255})
    print(f'PASS linked ELF, HEX, BIN and {blocks} nRF52840 UF2 blocks: {sum(marked)} programmed bytes, '
          f'{len(raw)-sum(marked)} alignment bytes, {len(raw)} binary bytes, final UF2 address {address+len(image):#x} exclusive, '
          f'{address+capacity-address-len(raw)} bytes raw application headroom; UF2 SHA-256 {hashlib.sha256(uf2).hexdigest()}')

def generated(directory,expected,linked):
    dts=(directory/'zephyr.dts').read_text()
    for name,region in expected.items():
        pattern=r'\b'+name+r':\s*partition@[\da-fA-F]+\s*\{([^{}]+)\}'
        assert pair(re.search(pattern,dts).group(1))==region,name
    assert not re.search(r'\bmt_pki_history:\s*partition@',dts)
    cfg=(directory/'.config').read_text().splitlines()
    assert 'CONFIG_MT_PKI_FRESH_DOMAIN_ONLY=y' in cfg
    assert ('CONFIG_MT_T096_FULL_COMPANION=y' in cfg)==(expected in (FULL,PAGED))
    assert ('CONFIG_MT_T096_PAGED_LAYOUT=y' in cfg)==(expected==PAGED)
    assert int(next(v.split('=',1)[1] for v in cfg if v.startswith('CONFIG_FLASH_LOAD_OFFSET=')),0)==expected['code_partition'][0]
    assert int(next(v.split('=',1)[1] for v in cfg if v.startswith('CONFIG_FLASH_LOAD_SIZE=')),0)==expected['code_partition'][1]
    print('PASS generated DTS physical reg values and .config flash load range for', directory)
    if linked:linked_files(directory,expected['code_partition'])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--paged-zephyr',type=Path,help='732 KiB paged build zephyr/ directory')
    parser.add_argument('--full-zephyr',type=Path,help='728 KiB build zephyr/ directory with generated DTS and .config')
    parser.add_argument('--original-zephyr',type=Path,help='680 KiB reset-capable build zephyr/ directory')
    parser.add_argument('--linked',action='store_true',help='also require and verify ELF, HEX, BIN and UF2 for every supplied directory')
    args=parser.parse_args()
    if args.linked and not(args.full_zephyr or args.original_zephyr or args.paged_zephyr):parser.error('--linked requires a build directory')
    with tempfile.TemporaryDirectory(prefix='zt-t096-layout-') as temp:
        tmp=Path(temp);source_maps();enrollment(tmp);floor(tmp)
    if args.original_zephyr:generated(args.original_zephyr,ORIGINAL,args.linked)
    if args.full_zephyr:generated(args.full_zephyr,FULL,args.linked)
    if args.paged_zephyr:generated(args.paged_zephyr,PAGED,args.linked)

if __name__=='__main__':main()
