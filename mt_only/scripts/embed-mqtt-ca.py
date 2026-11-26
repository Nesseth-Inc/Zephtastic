"""Validate one public TLS root and generate an immutable DER byte array."""
import hashlib
import sys
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

def read_root(path):
    with path.open('rb') as stream:
        data=stream.read(4097)
    if not 1<=len(data)<=4096:
        raise ValueError('MQTT root DER must be 1 to 4096 bytes')
    c=x509.load_der_x509_certificate(data)
    if c.public_bytes(serialization.Encoding.DER)!=data:
        raise ValueError('Expected a single canonical DER certificate')
    if not c.extensions.get_extension_for_class(x509.BasicConstraints).value.ca:
        raise ValueError('Expected a CA certificate')
    if not c.extensions.get_extension_for_class(x509.KeyUsage).value.key_cert_sign:
        raise ValueError('CA must permit certificate signing')
    key=c.public_key()
    if not (isinstance(key,rsa.RSAPublicKey) and 2048<=key.key_size<=4096 or
            isinstance(key,ec.EllipticCurvePublicKey) and isinstance(key.curve,ec.SECP256R1)):
        raise ValueError('Unsupported root public key on the XIAO TLS profile')
    c.verify_directly_issued_by(c)
    return data

if __name__=='__main__':
    try: data=read_root(Path(sys.argv[1]))
    except Exception:raise SystemExit('MQTT trust input must be one supported self-signed public DER CA with signing usage')
    fingerprint=hashlib.sha256(data).hexdigest()
    if len(sys.argv)>3 and fingerprint!=sys.argv[3]:
        raise SystemExit('Pinned MQTT root fingerprint mismatch')
    dest=Path(sys.argv[2]);dest.parent.mkdir(parents=True,exist_ok=True)
    body='\n'.join('  '+','.join(f'0x{x:02x}' for x in data[i:i+16])+',' for i in range(0,len(data),16))
    content='#pragma once\nstatic constexpr unsigned char mqttRootDer[] = {\n'+body+'\n};\n'
    if not dest.exists() or dest.read_text()!=content:dest.write_text(content)
    print('MQTT public CA SHA-256 '+fingerprint)
