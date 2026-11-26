# SPDX-License-Identifier: GPL-3.0-only
"""The MQTT build accepts one supported public, self-signed issuing root."""
from datetime import datetime,timedelta,timezone
from pathlib import Path
import hashlib,runpy,tempfile
from cryptography import x509
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

app=Path(__file__).resolve().parents[1]
read_root=runpy.run_path(str(app/'scripts/embed-mqtt-ca.py'))['read_root']
assert hashlib.sha256(read_root(app/'certs/isrg-root-x1.der')).hexdigest()=='96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6'
def cert(key,issuer,ca=True,sign=True):
    subject=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'fixture')])
    now=datetime.now(timezone.utc)
    return (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
            .public_key(key.public_key()).serial_number(1).not_valid_before(now-timedelta(minutes=1))
            .not_valid_after(now+timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=ca,path_length=None),critical=True)
            .add_extension(x509.KeyUsage(True,False,False,False,False,sign,False,False,False),critical=True)
            .sign(issuer,hashes.SHA256()).public_bytes(serialization.Encoding.DER))
key=ec.generate_private_key(ec.SECP256R1());aux=ec.generate_private_key(ec.SECP256R1())
p384=ec.generate_private_key(ec.SECP384R1());good=cert(key,key)
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'public.der';path.write_bytes(good);assert read_root(path)==good
    for bad in (b'', good+b'X', good+good, cert(key,key,False), cert(key,key,True,False),
                cert(key,aux),cert(p384,p384),bytes(4097),b'-----BEGIN PRIVATE KEY-----'):
        path.write_bytes(bad)
        try:read_root(path)
        except Exception:continue
        raise AssertionError('invalid root accepted')
print('PASS pinned X1, private P-256 CA and nine invalid build inputs')
