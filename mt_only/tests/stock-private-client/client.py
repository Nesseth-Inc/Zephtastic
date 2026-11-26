"""Actual stock Python API fixtures; definitions only, no transport or keys created."""
import base64
import hashlib
import importlib.metadata
import inspect
import json
from unittest.mock import patch
from meshtastic import mesh_interface
from meshtastic.mesh_interface import MeshInterface
from meshtastic.protobuf import mesh_pb2, portnums_pb2

CLIENT_VERSION = '2.7.11'


def wire(message):
    return message.SerializeToString(deterministic=True)


def provenance():
    assert importlib.metadata.version('meshtastic') == CLIENT_VERSION
    return dict(package='meshtastic', version=CLIENT_VERSION,
                mesh_interface_sha256=hashlib.sha256(inspect.getsource(mesh_interface).encode()).hexdigest(),
                mesh_schema_sha256=hashlib.sha256(mesh_pb2.DESCRIPTOR.serialized_pb).hexdigest(),
                knows_ack_proof_status='ack_proof_status' in mesh_pb2.MeshPacket.DESCRIPTOR.fields_by_name,
                physical_transport=False,packet_id_rng='fixed upper22 TEST bits; actual stock ID algorithm')


class StockClient(MeshInterface):
    def __init__(self, local, peer, peer_public=b''):
        super().__init__(timeout=1)
        self.currentPacketId=0x4d100000
        self.myInfo=mesh_pb2.MyNodeInfo(my_node_num=local)
        self.localNode.nodeNum=local
        self.localNode.localConfig.lora.hop_limit=3
        self.nodesByNum={node:dict(num=node,user=dict(id=f'!{node:08x}')) for node in (local,peer)}
        if peer_public:
            self.nodesByNum[peer]['user']['publicKey']=base64.b64encode(peer_public).decode()
        self.nodes={v['user']['id']:v for v in self.nodesByNum.values()}
        self.isConnected.set()
        self.sent=[]
        self.events=[]
        self.callbacks=[]
        self.configId=0x4d10ffff

    def _generatePacketId(self):
        # Pin only the stock algorithm's random input for reproducible host
        # vectors. This RNG substitution is never a firmware nonce provider.
        with patch.object(mesh_interface.random,'randint',return_value=0x134400):
            return super()._generatePacketId()

    def _sendToRadio(self, message):
        self.sent.append(wire(message))

    def accept(self, records):
        def publish(topic, **kwargs):
            self.events.append((topic,kwargs))
        with patch.object(mesh_interface.publishingThread,'queueWork',side_effect=lambda fn:fn()), \
             patch.object(mesh_interface.pub,'sendMessage',side_effect=publish):
            for raw in records:
                self._handleFromRadio(bytes.fromhex(raw) if isinstance(raw,str) else raw)


def build_requests(local, peer, peer_public):
    """Parameterized deterministic TEST public key; no private material is needed."""
    provenance()
    assert 1<local<0xfffffffe and 1<peer<0xfffffffe and local!=peer and len(peer_public)==32
    client=StockClient(local,peer,peer_public)
    cases=[]
    def collect(name, packet, shape, **extra):
        cases.append(dict(name=name, request_id=packet.id, toradio_hex=client.sent[-1].hex(),
                          shape=shape, **extra))
    collect('ordinary_direct_text',client.sendText('M10 deterministic private compatibility',destinationId=peer,wantAck=True),
            'stock sendText; PKI flag absent even with cached peer key', wants_private_when_runtime_ready=True)
    collect('ordinary_direct_reply',client.sendText('M10 reply',destinationId=peer,wantAck=True,replyId=0x4d001122),
            'stock sendText with replyId', reply_id=0x4d001122,wants_private_when_runtime_ready=True)
    collect('explicit_private_key',client.sendData(b'M10 explicit private',destinationId=peer,portNum=portnums_pb2.TEXT_MESSAGE_APP,
            wantAck=True,pkiEncrypted=True,publicKey=peer_public),'stock sendData explicit PKI and matching key')
    collect('explicit_private_no_client_key',client.sendData(b'M10 private resolve key',destinationId=peer,
            portNum=portnums_pb2.TEXT_MESSAGE_APP,wantAck=True,pkiEncrypted=True),'stock sendData explicit PKI with runtime key lookup')
    wrong=bytes([peer_public[0]^0x80])+peer_public[1:]
    collect('explicit_private_key_mismatch',client.sendData(b'M10 mismatch must fail',destinationId=peer,
            portNum=portnums_pb2.TEXT_MESSAGE_APP,wantAck=True,pkiEncrypted=True,publicKey=wrong),
            'stock sendData explicit mismatching client public key',expected_error='PKI_FAILED')
    collect('self_private_request',client.sendData(b'M10 local echo is not authenticated',destinationId=local,
            portNum=portnums_pb2.TEXT_MESSAGE_APP,wantAck=True,pkiEncrypted=True,publicKey=peer_public),
            'stock sendData self destination; must not echo caller authentication claims')
    forged=mesh_pb2.MeshPacket()
    forged.decoded.portnum=portnums_pb2.TEXT_MESSAGE_APP
    forged.decoded.payload=b'M10 local proof claim must clear'
    # Unknown to this client version, but parsable/preserved in its raw protobuf.
    forged.ParseFromString(wire(forged)+b'\xb8\x01\x01')
    collect('self_forged_ack_proof',client._sendPacket(forged,destinationId=local,wantAck=True),
            'stock raw MeshPacket API preserving unknown2.8 tag23; must clear unverified proof')
    collect('private_application_response',client.sendData(b'M10 app response unsupported',destinationId=peer,
            portNum=portnums_pb2.TEXT_MESSAGE_APP,wantAck=True,wantResponse=True,pkiEncrypted=True,publicKey=peer_public),
            'stock sendData wants application response; bounded endpoint unsupported')
    collect('private_secondary_channel',client.sendData(b'M10 unsupported channel',destinationId=peer,
            portNum=portnums_pb2.TEXT_MESSAGE_APP,wantAck=True,pkiEncrypted=True,publicKey=peer_public,channelIndex=1),
            'stock sendData nonprimary channel',expected_error='NO_CHANNEL')
    collect('explicit_private_broadcast',client.sendData(b'M10 broadcast cannot be private',destinationId=0xffffffff,
            portNum=portnums_pb2.TEXT_MESSAGE_APP,wantAck=True,pkiEncrypted=True,publicKey=peer_public),
            'stock sendData explicit PKI broadcast; reject without public fallback')
    for case in cases:
        request=mesh_pb2.ToRadio.FromString(bytes.fromhex(case['toradio_hex']))
        assert request.packet.id==case['request_id'] and request.packet.decoded.portnum==portnums_pb2.TEXT_MESSAGE_APP
        assert request.packet.hop_limit==3 and request.packet.want_ack
        if case['name'].startswith('ordinary_'):
            assert not request.packet.pki_encrypted and not request.packet.public_key
    return cases


def raw_varint_field(data, tag):
    """Read unknown enum metadata without pretending the installed client knows it."""
    offset=0;result=[]
    def integer():
        nonlocal offset
        value=0
        for shift in range(0,70,7):
            assert offset<len(data);b=data[offset];offset+=1;value|=(b&127)<<shift
            if b<128:return value
        raise AssertionError('Oversized varint')
    while offset<len(data):
        key=integer();field,kind=key>>3,key&7;assert field
        if kind==0:
            value=integer()
            if field==tag:result.append(value)
        elif kind==1:offset+=8
        elif kind==5:offset+=4
        elif kind==2:
            length=integer();offset+=length
        else:raise AssertionError('Unsupported wire type')
        assert offset<=len(data)
    return result


def inspect_records(records, local, peer, peer_public=b''):
    """Return real-client events; do not infer authentication from packet shape."""
    client=StockClient(local,peer,peer_public)
    client.accept(records)
    packets=[kwargs['packet'] for topic,kwargs in client.events if topic.startswith('meshtastic.receive')]
    return client,packets


if __name__=='__main__':
    import argparse
    from pathlib import Path
    parser=argparse.ArgumentParser(description='Generate host-only stock client request fixtures from public TEST data')
    parser.add_argument('--local',required=True,type=lambda x:int(x,0))
    parser.add_argument('--peer',required=True,type=lambda x:int(x,0))
    parser.add_argument('--peer-public-key',required=True,type=bytes.fromhex)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    args.output.write_text(json.dumps(dict(provenance=provenance(),cases=build_requests(args.local,args.peer,args.peer_public_key)),indent=2)+'\n')
