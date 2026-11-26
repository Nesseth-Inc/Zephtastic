#!/usr/bin/env python3
"""Build the full XIAO and its public-key-only MCUboot, seal four disposable versions in memory.

This compile-only qualification package uses a fresh in-memory key. All products are public.
The full erased-device image always requires a separate, explicitly authorized first installation.
"""
import argparse
import json
from pathlib import Path
import resource
import sys
from imgtool import image, keys
from imgtool.version import decode_version

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from xiao_release import FLASH_SIZE as SIZE, SECONDARY, PRIMARY, SLOT, sha, build_images

PERSISTENT = [(0x390000, 0x3e0000), (0x3f0000, SIZE)]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('zephyr','modules','mcuboot','sdk','build','out'):
        p.add_argument('--'+name,required=True,type=Path)
    p.add_argument('--source-date-epoch',required=True,type=int)
    p.add_argument('--ble',action='store_true',help='Require compiled stock Bluetooth carrier support')
    a=p.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    root,out=a.build.resolve(),a.out.resolve()
    out.mkdir(mode=0o700,parents=True,exist_ok=False);root.mkdir(mode=0o700,parents=True,exist_ok=True)
    k=keys.ECDSA256P1.generate()
    pub=out/'trust-public.pem';pub.write_bytes(k.get_public_pem())
    raw,loader=build_images(root,a.zephyr,a.modules,a.mcuboot,a.sdk,pub,a.source_date_epoch,require_ble=a.ble)
    rawfile=root/'app/zephyr/zephyr.bin'
    obj={'format':1,'board':'xiao_esp32s3/esp32s3/procpu','full_ordinary_companion':True,
         'fresh_layout_only':True,'qualification_signing_key':'ephemeral P-256; private key discarded',
         'key_der_sha256':sha(k.get_public_bytes()),'raw_application_sha256':sha(raw),'items':{}}
    def record(f,**fields):obj['items'][f.name]={'bytes':f.stat().st_size,'sha256':sha(f.read_bytes()),**fields}
    record(pub)
    bootfile=out/'mcuboot-xiao.bin';bootfile.write_bytes(loader);record(bootfile,offset=0)
    factory=bytearray(b'\xff'*SIZE);factory[:len(loader)]=loader
    for label,build in zip('ABCD',range(100,104)):
        ver='2.8.1+'+str(build)
        def seal(target,*,pad=False,confirm=False,test=False):
            candidate=image.Image(version=decode_version(ver),header_size=32,align=4,max_align=8,
                slot_size=SLOT,max_sectors=448,pad=pad,confirm=confirm,test=test)
            candidate.load(str(rawfile));candidate.create(k,'hash',None);candidate.save(str(target))
            rc,v,h,_=image.Image.verify(str(target),k)
            if rc!=image.VerifyResult.OK or tuple(v)!=tuple(decode_version(ver)):raise SystemExit('Signature check failed')
            return h.hex(),candidate
        if label=='A':
            f=out/'initial-A.signed.bin';h,_=seal(f,pad=True,confirm=True)
            assert f.stat().st_size==SLOT and f.read_bytes()[-24]==1
            factory[PRIMARY:PRIMARY+SLOT]=f.read_bytes(); record(f,version=ver,offset=PRIMARY,mcuboot_digest=h);continue
        f=out/('firmware-xiao-'+label+'-update.bin');h,signed=seal(f);record(f,version=ver,mcuboot_digest=h,carrier='wifi-or-ble')
        if label=='D':
            pre=f.read_bytes();f=out/'firmware-xiao-D-wired-update.bin'
            signed.pad=True;signed.test=True;signed.save(str(f))
            assert f.read_bytes()[:len(pre)]==pre and image.Image.verify(str(f),k)[0]==image.VerifyResult.OK
            assert f.stat().st_size==SLOT and f.read_bytes()[-24]==255
            record(f,version=ver,mcuboot_digest=h,offset=SECONDARY,carrier='stock-device-update.sh')
    for start,end in PERSISTENT:assert factory[start:end]==b'\xff'*(end-start)
    f=out/'xiao-fresh-signed-8m.factory.bin';f.write_bytes(factory);record(f,offset=0)
    (out/'manifest.json').write_text(json.dumps(obj,indent=2)+'\n')
if __name__=='__main__':main()
