# SPDX-License-Identifier: GPL-3.0-only
"""Create disposable, two-party PKI fixtures. Only ciphertext and public keys leave this function."""
import hashlib
import re
import struct

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
from meshtastic.protobuf import mesh_pb2, portnums_pb2


def write_vectors(test, output):
    def array(data):
        return "{" + ",".join(str(value) for value in data) + "}"

    sender, recipient = X25519PrivateKey.generate(), X25519PrivateKey.generate()
    key = hashlib.sha256(sender.exchange(recipient.public_key())).digest()
    peer_key = hashlib.sha256(recipient.exchange(sender.public_key())).digest()
    keys = [identity.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw) for identity in (sender, recipient)]
    rows = []
    for port, hex_id in re.findall(r"makePkiUnicastBetween\(admin, target, meshtastic_PortNum_(\w+), (0x[0-9A-Fa-f]+)", test):
        packet_id = int(hex_id, 16)
        value = getattr(portnums_pb2, port)
        plaintext = mesh_pb2.Data(portnum=value, payload=b"fixture").SerializeToString()
        # The policy test gives each ingress its own ID. Encrypt for each
        # actual ID so the frame remains valid for its intended recipient.
        offsets = (0, 256, 512, 768) if packet_id == 0xADAA000A else (0,)
        for offset in offsets:
            wire_id = packet_id + offset
            extra = len(rows) + 1
            nonce = struct.pack("<IIIB", wire_id, extra, 0x0C0C0C0C, 0)
            sealed = AESCCM(key, tag_length=8).encrypt(nonce, plaintext, None)
            if AESCCM(peer_key, tag_length=8).decrypt(nonce, sealed, None) != plaintext:
                raise ValueError("Recipient could not recover the fixture")
            wire = sealed + struct.pack("<I", extra)
            rows.append(f"{{{hex(wire_id)}, {value}, {len(wire)}, {array(wire)}}}")
    if not rows:
        raise ValueError("No directed PKI cases found")
    output.write_text("const uint8_t senderPublic[32]=" + array(keys[0]) +
                      ";\nconst uint8_t recipientPublic[32]=" + array(keys[1]) +
                      ";\nconst struct {uint32_t id,port,size; uint8_t body[64];} vectors[]={\n" +
                      ",\n".join(rows) + "\n};\n")
