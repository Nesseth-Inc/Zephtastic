# SPDX-License-Identifier: GPL-3.0-only
"""Read actual CDC records into stock MeshInterface while a simulated beacon remains stuck."""
import json
from pathlib import Path
import select
import subprocess
import sys
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'stock-private-client'))
from client import StockClient, mesh_interface, mesh_pb2, portnums_pb2, provenance
from meshtastic.protobuf import admin_pb2

SELF=0x12345678
PEER=0x87654321

def framed(message):
    data=message.SerializeToString() if hasattr(message,'SerializeToString') else message
    return b'\x94\xc3'+len(data).to_bytes(2,'big')+data

def request(packet_id,**admin):
    message=mesh_pb2.ToRadio()
    p=message.packet
    p.id=packet_id
    p.to=SELF
    p.want_ack=True
    p.decoded.portnum=portnums_pb2.ADMIN_APP
    p.decoded.want_response=True
    p.decoded.payload=admin_pb2.AdminMessage(**admin).SerializeToString()
    return message

def session(exe,env,fault):
    with subprocess.Popen([exe,fault],stdin=subprocess.PIPE,stdout=subprocess.PIPE,env=env,text=True) as proc:
        def cmd(s):
            proc.stdin.write(s+'\n')
            proc.stdin.flush()
            if not select.select([proc.stdout],[],[],15)[0]:
                raise TimeoutError(f'USB {fault} failed to respond to {s.split()[0]}')
            row=json.loads(proc.stdout.readline())
            assert row['saves']==0
            return row
        def queue(data):
            cmd('queue '+data.hex())
        def poll():
            return cmd('poll')['records']
        def snapshot(nonce,restricted):
            client=StockClient(SELF,PEER)
            client.isConnected.clear()
            client.myInfo=None
            client.nodesByNum={}
            client.nodes={}
            with patch.object(mesh_interface.random,'randint',return_value=nonce):
                client._startConfig()
            wire=client.sent[-1]
            assert mesh_pb2.ToRadio.FromString(wire).want_config_id==nonce
            queue(framed(wire))
            records=[]
            for _ in range(160):
                chunk=poll()
                if restricted:
                    assert len(chunk)<=4, 'recovery exceeded its per-poll snapshot budget'
                client.accept(chunk)
                records.extend(mesh_pb2.FromRadio.FromString(bytes.fromhex(raw)) for raw in chunk)
                if any(r.HasField('config_complete_id') for r in records):
                    break
            assert [r.config_complete_id for r in records if r.HasField('config_complete_id')]==[nonce]
            assert records[0].my_info.my_node_num==SELF
            assert [r.channel.index for r in records if r.HasField('channel')]==list(range(8))
            assert any(r.HasField('node_info') and r.node_info.num==SELF for r in records)
            assert any(r.HasField('metadata') for r in records)
            assert any(r.HasField('config') and r.config.HasField('device') for r in records)
            assert any(r.HasField('moduleConfig') for r in records)
            assert client.isConnected.is_set() and client.myInfo.my_node_num==SELF and SELF in client.nodesByNum
            assert len(client.localNode.channels)==8
            return client
        def packets(records):
            envelopes=[mesh_pb2.FromRadio.FromString(bytes.fromhex(raw)) for raw in records]
            return [r.packet for r in envelopes if r.HasField('packet')]
        def query(client,i):
            for tag,expected in [('get_device_metadata_request','get_device_metadata_response'),
                                 ('get_device_connection_status_request','get_device_connection_status_response')]:
                q=request(i,**{tag:True})
                queue(framed(q)); rows=poll();client.accept(rows)
                hits=[p for p in packets(rows) if p.decoded.request_id==i]
                assert len(hits)==1
                p=hits[0]
                assert getattr(p,'from')==p.to==SELF and p.decoded.portnum==portnums_pb2.ADMIN_APP
                response=admin_pb2.AdminMessage.FromString(p.decoded.payload)
                assert response.HasField(expected)
                if tag=='get_device_metadata_request':
                    assert response.get_device_metadata_response.hw_model==127
                else:
                    assert response.get_device_connection_status_response.serial.is_connected
                i+=1
            heartbeat=mesh_pb2.ToRadio()
            heartbeat.heartbeat.nonce=1
            queue(framed(heartbeat)); rows=poll();client.accept(rows)
            assert any(mesh_pb2.FromRadio.FromString(bytes.fromhex(raw)).HasField('queueStatus') for raw in rows)
        def denied(rows,ids):
            found={}
            for p in packets(rows):
                if p.decoded.request_id in ids:
                    assert p.decoded.portnum==portnums_pb2.ROUTING_APP
                    assert mesh_pb2.Routing.FromString(p.decoded.payload).error_reason==mesh_pb2.Routing.NO_INTERFACE
                    found[p.decoded.request_id]=1
            assert sorted(found)==sorted(ids)

        existing=snapshot(71,False)
        assert cmd('fault')['recovering']
        count=0
        for client in (existing,None):
            if client is None:
                client=snapshot(73,True)
            for i in range(3):
                assert cmd('retry')['recovering']
                query(client,100+count*10)
                count+=1
        # Route-to-air and a local setter must return correlated denials without invoking a host write.
        air=mesh_pb2.ToRadio()
        air.packet.id=910;air.packet.to=PEER
        air.packet.decoded.portnum=portnums_pb2.TEXT_MESSAGE_APP
        air.packet.decoded.payload=b'fixture'
        setter=lambda i: framed(request(i,set_owner=mesh_pb2.User(long_name='Fixture change')))
        queue(framed(air)+setter(911))
        denied(poll(),[910,911])
        # The next DTR epoch cannot inherit the old client's incomplete request.
        split=setter(912)
        queue(split[:8]);assert not packets(poll())
        cmd('epoch');queue(split[8:]);assert not packets(poll())
        fresh=snapshot(75,True)
        # A partially parsed recovery command and another complete command still
        # in the CDC ring both retain their original admission across release.
        split=setter(913)
        queue(split[:8]);assert not packets(poll())
        queue(split[8:]+setter(914))
        assert not cmd('release')['recovering']
        denied(poll()+poll(),[913,914])
        queue(framed(request(915,get_owner_request=True)))
        rows=poll();fresh.accept(rows)
        assert any(p.decoded.request_id==915 and p.decoded.portnum==portnums_pb2.ADMIN_APP and
                   admin_pb2.AdminMessage.FromString(p.decoded.payload).HasField('get_owner_response') for p in packets(rows))
        proc.stdin.close()
        assert proc.wait(timeout=10)==0
    return {'stock_handshakes':3,'existing_session_read_cycles':3,'fresh_session_read_cycles':3,
            'metadata_status_replies':2*count,'heartbeat_replies':count,
            'denied_writes_and_sends':4,'epoch_split_rejected':True,'ordinary_read_after_release':True}

def verify_usb(exe,env):
    provenance()
    return {fault:session(exe,env,fault) for fault in ('lost','home')}
