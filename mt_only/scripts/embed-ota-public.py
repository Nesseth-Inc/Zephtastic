#!/usr/bin/env python3
"""Convert a P-256 SubjectPublicKeyInfo PUBLIC KEY to the two MCUboot trust values."""
import base64
import hashlib
from pathlib import Path
import sys

pem = Path(sys.argv[1]).read_bytes().strip().splitlines()
if len(pem)<3 or pem[0]!=b'-----BEGIN PUBLIC KEY-----' or pem[-1]!=b'-----END PUBLIC KEY-----':
    raise SystemExit('A P-256 PUBLIC KEY is required')
der = base64.b64decode(b''.join(pem[1:-1]), validate=True)
prefix = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200')
point = der[len(prefix):]
if not der.startswith(prefix) or len(point)!=65 or point[0]!=4:
    raise SystemExit('Require uncompressed P-256 SPKI')
header = '#pragma once\n#include <stdint.h>\n'
for name, value in [('releaseKeyHash',hashlib.sha256(der).digest()),('releasePoint',point)]:
    header += f'static constexpr uint8_t {name}[]={{' + ','.join(str(x) for x in value) + '};\n'
Path(sys.argv[2]).write_text(header)
