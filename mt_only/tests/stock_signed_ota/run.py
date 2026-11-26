#!/usr/bin/env python3
"""Run the firmware's real Session against a NOR owner model and OpenSSL P-256."""
import hashlib, os, struct, subprocess, sys, tempfile
from pathlib import Path
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec,utils

root=Path(__file__).resolve().parent;mt=root.parents[1]
def make(key,version):
    payload=b'ZT stock-OTA host fixture\n'*196
    header=struct.pack('<IIHHIIBBHII',0x96f3b83d,0,32,0,len(payload),0,2,8,1,version,0)
    body=header+payload;d=hashlib.sha256(body).digest()
    pub=key.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo)
    sig=key.sign(d,ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    def tlv(kind,v):return struct.pack('<HH',kind,len(v))+v
    tlvs=tlv(0x10,d)+tlv(1,hashlib.sha256(pub).digest())+tlv(0x22,sig)
    return body+struct.pack('<HH',0x6907,4+len(tlvs))+tlvs
with tempfile.TemporaryDirectory() as temp:
    w=Path(temp);key=ec.generate_private_key(ec.SECP256R1())
    (w/'pub.pem').write_bytes(key.public_key().public_bytes(serialization.Encoding.PEM,serialization.PublicFormat.SubjectPublicKeyInfo))
    for name,signer,v in [('a',key,100),('b',key,101),('foreign',ec.generate_private_key(ec.SECP256R1()),102)]:
        (w/name).write_bytes(make(signer,v))
    subprocess.run([sys.executable,mt/'scripts/embed-ota-public.py',w/'pub.pem',w/'MtReleaseKey.h'],check=True)
    proto=mt.parent/'zephcore/bench/upstream-differential/source'
    (w/'meshtastic').symlink_to(proto,target_is_directory=True)
    openssl=os.environ.get('OPENSSL_PREFIX')
    flags=(['-I'+openssl+'/include','-L'+openssl+'/lib','-Wl,-rpath,'+openssl+'/lib'] if openssl else [])
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wno-deprecated-declarations','-DCONFIG_MT_SIGNED_WIFI_OTA=1','-DCONFIG_BOARD_XIAO_ESP32S3=1',
        '-I'+str(w),'-I'+str(proto/'nanopb'),'-I'+str(root/'include'),'-I'+str(mt/'src/update'),*flags,
        root/'host.cpp',root/'stream.cpp',mt/'src/update/SignedOta.cpp','-lcrypto','-o',w/'host'],check=True)
    subprocess.run([w/'host',w/'a',w/'b',w/'foreign'],check=True)
