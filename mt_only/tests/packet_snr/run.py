#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bounded native-status, packet-queue and real nanopb SNR host qualification."""
if not __debug__:
    raise RuntimeError('Optimized packet SNR qualification refused')
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sys

HERE=Path(__file__).resolve().parent
APP=HERE.parents[1]
SOURCE=APP.parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',required=True,type=Path)
parser.add_argument('--short-enums',action='store_true')
args=parser.parse_args()
output=args.output.resolve()
if output==SOURCE or SOURCE in output.parents or output.exists():
    parser.error('New output outside the source tree required')
sys.dont_write_bytecode=True
sys.path.insert(0,str(APP/'tests/airtime_quality'))
from bounded_process import run_group,install_termination_handler
install_termination_handler()

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def extract(text,start):
    pos=text.index(start);opening=text.index('{',pos);depth=1;i=opening+1
    while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[pos:i]
def compiler(name,default):
    argv=shlex.split(os.environ.get(name,default))
    if not argv or shutil.which(argv[0]) is None:raise ValueError('Compiler required')
    argv[0]=str(Path(shutil.which(argv[0])).resolve())
    return argv

vendor=SOURCE/'zephcore/adapters/bench/vendor'
proto=SOURCE/'zephcore/bench/upstream-differential/source'
mono=SOURCE/'zephcore/lib/monocypher'
driver=APP/'dependency-overlay/zephyr/drivers/lora/native/sx126x'
ext=APP/'dependency-overlay/zephyr/include/zephyr/drivers/lora/sx126x_ext.h'
files=list((APP/'src').rglob('*.h'))+list((APP/'src').rglob('*.hpp'))+[APP/'src/Protobuf.cpp',APP/'src/main.cpp']
# These donor sources are compiled from Protobuf.cpp in the production unit.
files.extend(APP/'src'/relative for relative in re.findall(r'^#include "([^"]+\.cpp)"',
    (APP/'src/Protobuf.cpp').read_text(),re.M))
files+=list(vendor.glob('*.h'))+[vendor/'MtCodec.cpp',vendor/'MtSignature.cpp']
files+=list(proto.glob('*.pb.h'))+list((proto/'nanopb').glob('*.[ch]'))
files+=[mono/n for n in ('monocypher.h','monocypher-ed25519.h','monocypher.c','monocypher-ed25519.c')]
files+=[driver/'sx126x.c',driver/'sx126x.h',driver/'sx126x_regs.h',driver/'sx126x_airtime_math.h',ext]
files+=[APP/'tests/channel_metrics/native/prefix.c',APP/'tests/peer_last_heard/export_cases.cpp',APP/'tests/airtime_quality/bounded_process.py']
files+=list(HERE.glob('*.c'))+list(HERE.glob('*.cpp'))+[Path(__file__).resolve(),HERE/'README.md']
pins={str(p.relative_to(SOURCE)):sha(p) for p in sorted(set(files))}
cc=compiler('CC','cc');cxx=compiler('CXX','c++')
output.mkdir(parents=True)
receipt=dict(status='FAIL',source_sha256=pins,short_enums=args.short_enums,
    started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),commands=[],
    hardware=False,network=False,RF=False,sanitizers=['address','undefined'],
    boundary='synthetic HAL/RTOS queues and private admission; actual native bodies/Service/Companion/nanopb; no target scheduler/cryptographic/physical claim')
environment=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1')
def run(argv,name,seconds):
    entry=dict(argv=[str(a) for a in argv],log=name,timeout_seconds=seconds);receipt['commands'].append(entry)
    try:
        with (output/name).open('x') as stream:run_group(entry['argv'],timeout=seconds,stdout=stream,env=environment)
        entry.update(reaped=True,group_gone=True)
    except BaseException as error:
        entry['cleanup']=getattr(error,'owned_process_cleanup',{});raise
try:
    frozen=output/'frozen-source'
    for name,pin in pins.items():
        dest=frozen/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(SOURCE/name,dest)
        assert sha(dest)==pin
    app=frozen/'mt_only';vendor=frozen/'zephcore/adapters/bench/vendor';proto=frozen/'zephcore/bench/upstream-differential/source';mono=frozen/'zephcore/lib/monocypher'
    drv=app/'dependency-overlay/zephyr/drivers/lora/native/sx126x'
    native=(drv/'sx126x.c').read_text();extended=(app/'dependency-overlay/zephyr/include/zephyr/drivers/lora/sx126x_ext.h').read_text()
    status=extract(extended,'struct sx126x_packet_status {')+';'
    callback=extended[extended.index('typedef void (*sx126x_recv_packet_cb)'):];callback=callback[:callback.index(';')+1]
    prefix=(app/'tests/channel_metrics/native/prefix.c').read_text()
    prefix=prefix.replace('/* STAGE_INFO_STRUCT */',extract(extended,'struct sx126x_stage_info {')+';')
    prefix=prefix.replace('/* PACKET_STATUS_DECLS */',status+'\n'+callback)
    prefix=prefix.replace('/* OBSERVATION_STRUCT */',extract(extended,'struct sx126x_airtime_observation {')+';')
    math=(drv/'sx126x_airtime_math.h').read_text();function=re.search(r'(?:static\s+inline\s+)?struct sx126x_airtime_value\s+sx126x_airtime_estimate\s*\(',math)
    prefix=prefix.replace('/* AIRTIME_FUNCTION */',extract(math,function.group(0)))
    a=prefix.index('/* Both historical three-argument');b=prefix.index('static int sx126x_hal_read_buffer',a)
    prefix=prefix[:a]+prefix[b:]
    prefix=prefix.replace('static uint8_t input_length=16;','static uint8_t input_length=16,payload_tag=0x25;')
    prefix=prefix.replace('memset(buf,0x25,n)','memset(buf,payload_tag,n)')
    prefix+='\n'+next(line for line in (drv/'sx126x_regs.h').read_text().splitlines() if line.startswith('#define SX126X_CMD_GET_PACKET_STATUS '))+'\n'
    prefix+='''static uint8_t packet_raw;static unsigned packet_reads;
static int sx126x_hal_read_cmd(const struct device *d,uint8_t cmd,uint8_t *buf,size_t n){(void)d;assert(cmd==SX126X_CMD_GET_PACKET_STATUS&&n==3);++packet_reads;mark('p');buf[0]=180;buf[1]=packet_raw;buf[2]=0;return packet_error;}
static void sx126x_observe_rx(struct sx126x_data *d,uint16_t i,uint8_t n,bool k){(void)d;(void)i;(void)n;(void)k;}
'''
    names=['static int sx126x_get_packet_status(','static int sx126x_recv_async_common(','static int sx126x_lora_recv_async(','static void sx126x_packet_callback_marker(','int sx126x_recv_async_packet(','static int sx126x_handle_irq_rx_done(']
    generated=output/'native.c';generated.write_text(prefix+'\n'+'\n'.join(extract(native,n) for n in names)+'\n'+(app/'tests/packet_snr/native_cases.c').read_text())
    cflags=['-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']+(['-fshort-enums'] if args.short_enums else [])
    run([*cc,'-std=c11',*cflags,'-pthread',generated,'-o',output/'test_native'],'native.build.log',60)
    run([output/'test_native'],'native.log',20)
    main=(app/'src/main.cpp').read_text()
    (output/'queue_callback.h').write_text(status+'\n'+extract(main,'struct Rx {')+';\n'+extract(main,'static void receive('))
    expected='(event.length?event.data:nullptr,event.length,event.rssi,event.snr,receiveNow,receiveUtc,event.snrKnown,event.snrRaw,true,companionWorkspaceProof());'
    assert main.count('companion.receive'+expected)==1 and main.count('bleCompanion.receive'+expected)==1
    assert 'service.rx(event.length?event.data:nullptr,event.length,event.snr,receiveNow,sys_rand32_get(),true,&accepted,false,' in main
    assert 'receiveUtc,uint32_t(receiveUptime/1000u),event.rssi,event.snrKnown?mt::packetSnrDb(event.snrRaw):float(event.snr));' in main
    run([*cxx,'-std=c++17',*cflags,'-I'+str(output),app/'tests/packet_snr/queue_cases.cpp','-o',output/'test_queue'],'queue.build.log',60)
    run([output/'test_queue'],'queue.log',15)
    fixture=(app/'tests/peer_last_heard/export_cases.cpp').read_text()
    (output/'export_fixture.h').write_text(fixture[:fixture.index('int main(){')])
    (output/'meshtastic').mkdir()
    for header in proto.glob('*.pb.h'):shutil.copyfile(header,output/'meshtastic'/header.name)
    includes=['-I'+str(app/'src'),'-I'+str(vendor),'-I'+str(output),'-I'+str(proto/'nanopb'),'-I'+str(mono)]
    if sys.platform.startswith('linux'):includes+=['-DMT_HOST_OPENSSL=1']
    if sys.platform=='darwin':includes+=['-DEKEYREJECTED=129']
    objects=[]
    for source in [proto/'nanopb'/n for n in ('pb_common.c','pb_encode.c','pb_decode.c')]+[mono/'monocypher.c',mono/'monocypher-ed25519.c']:
        obj=output/(source.name+'.o');run([*cc,'-std=c11',*cflags,*includes,'-c',source,'-o',obj],source.name+'.build.log',90);objects.append(obj)
    link=[*cxx,'-std=c++17',*cflags,*includes,app/'tests/packet_snr/export_cases.cpp',app/'src/Protobuf.cpp',vendor/'MtCodec.cpp',vendor/'MtSignature.cpp',*objects]
    if sys.platform.startswith('linux'):link+=['-lcrypto']
    run([*link,'-o',output/'test_exports'],'exports.build.log',120)
    run([output/'test_exports'],'exports.log',30)
    receipt['groups']={name:[s for s in (output/(name+'.log')).read_text().splitlines() if s.startswith('PASS ')] for name in ('native','queue','exports')}
    assert [len(receipt['groups'][n]) for n in ('native','queue','exports')]==[6,2,3]
    receipt.update(status='PASS',source_unchanged=all(sha(SOURCE/n)==pin for n,pin in pins.items()),frozen_unchanged=all(sha(frozen/n)==pin for n,pin in pins.items()),actual_native_bodies=names,main_fanouts_same_packet_metadata=True)
    assert receipt['source_unchanged'] and receipt['frozen_unchanged']
except BaseException as error:
    receipt.update(status='FAIL',error_type=type(error).__name__,error=str(error))
finally:
    receipt['ended_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    (output/'SUMMARY.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({k:receipt[k] for k in ('status','short_enums')},sort_keys=True))
raise SystemExit(receipt['status']!='PASS')
