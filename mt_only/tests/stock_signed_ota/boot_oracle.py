#!/usr/bin/env python3
"""Exercise a released MCUboot binary against synthetic flash using Espressif ESP32-S3 QEMU."""
import argparse, hashlib, json, os, struct, subprocess, tempfile, time
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('artifacts',type=Path);p.add_argument('emulator',type=Path)
args=p.parse_args();base=args.artifacts.resolve();em=args.emulator.resolve()
manifest=json.loads((base/'manifest.json').read_text())['items']
def artifact(name):
    buf=(base/name).read_bytes()
    if hashlib.sha256(buf).hexdigest()!=manifest[name]['sha256']:raise SystemExit('Invalid package digest')
    return buf
def extent(buf):
    off=struct.unpack_from('<H',buf,8)[0]+struct.unpack_from('<I',buf,12)[0]+struct.unpack_from('<H',buf,10)[0]
    return buf[:off+struct.unpack_from('<H',buf,off+2)[0]]
def digest(b):return hashlib.sha256(b).digest()
factory=bytearray(artifact('xiao-fresh-signed-8m.factory.bin'))
# Synthesize non-erased canaries at each protected sector boundary. Stage only
# in QEMU so an unintended mass erase would fail the final comparison.
for lo,hi in [(0x390000,0x3e0000),(0x3f0000,0x800000)]:
    for offset in range(lo,hi,4096):
        stamp=hashlib.sha256(b'ZT synthetic preserved sector'+struct.pack('<I',offset)).digest()
        factory[offset:offset+len(stamp)]=stamp
A=artifact('initial-A.signed.bin');D=artifact('firmware-xiao-D-wired-update.bin')
end=0x1d0000;primary=end;sec=0x10000;slot=0x1c0000
assert len(A)==len(D)==slot and factory[primary:primary+slot]==A
assert A[-24]==1 and D[-24]==255 and A[-16:]==D[-16:]
env={**os.environ,'LD_LIBRARY_PATH':':'.join(str(em/t) for t in ('libroot/usr/lib/x86_64-linux-gnu','libroot/usr/lib/x86_64-linux-gnu/pulseaudio'))}
ledger=[]
with tempfile.TemporaryDirectory() as td:
    flash=Path(td)/'simulated-nor.bin'
    def boot(initial,pv,sv,expected_confirmed,case,score):
        flash.write_bytes(initial);dead=time.monotonic()+55;success=False
        error=Path(td)/'qemu.stderr'
        err=error.open('wb')
        proc=subprocess.Popen([str(em/'qemu/bin/qemu-system-xtensa'),'-M','esp32s3','-L',str(em/'qemu/share/qemu'),
            '-display','none','-serial','null','-monitor','none','-nic','none','-drive',
            'file='+str(flash)+',if=mtd,format=raw,cache=writeback'],env=env,stdout=subprocess.DEVNULL,stderr=err)
        try:
            while time.monotonic()<dead and proc.poll() is None:
                time.sleep(.20);raw=flash.read_bytes();one=raw[primary:primary+slot];two=raw[sec:end]
                try:
                    success=digest(extent(one))==pv and (sv is None or digest(extent(two))==sv) and score(one,two) and (one[-24]==1)==expected_confirmed
                except (struct.error,IndexError):success=False
                if success:break
        finally:
            if proc.poll() is None:proc.terminate()
            proc.wait(timeout=5)
        err.close();actual=flash.read_bytes()
        if not success:
            print('QEMU diagnostic',proc.returncode,error.read_text(errors='replace')[:240],
                  'primary',actual[primary:primary+32].hex(),'secondary',actual[sec:sec+32].hex(),
                  'trailers',actual[primary+slot-40:primary+slot].hex(),actual[end-40:end].hex(),flush=True)
        assert actual[0x390000:0x3e0000]==factory[0x390000:0x3e0000] and actual[0x3f0000:]==factory[0x3f0000:]
        ledger.append(dict(case=case,success=success,primary_confirmed=expected_confirmed,high_storage_unchanged=True))
        if not success:raise SystemExit(json.dumps(ledger))
        return actual
    staged=bytearray(factory);staged[sec:end]=D
    exA,exD=digest(extent(A)),digest(extent(D))
    trial=boot(staged,exD,exA,False,'stock wired D staged at 0x10000',lambda a,b:a[-32]==1)
    boot(trial,exA,exD,True,'unconfirmed D reverts to verified A',lambda a,b:a[-32]==1)
    # Reuse an authentic old signed image under a confirmed newer primary.
    old=bytearray(factory);old[primary:primary+slot]=D;old[primary+slot-24]=1
    old[sec:end]=A;old[end-24]=255
    boot(old,exD,None,True,'signed downgrade denied',lambda a,b:b[:4]!=A[:4] or b[-16:]!=A[-16:])
    bad=bytearray(staged);bad[sec+300]^=1
    boot(bad,exA,None,True,'corrupt signed secondary denied',lambda a,b:b[:4]!=D[:4] or b[-16:]!=D[-16:])
print(json.dumps({'MCUboot':manifest['mcuboot-xiao.bin']['sha256'],'emulated':True,'cases':ledger},indent=2))
