#!/usr/bin/env python3
"""Loopback-only UI fixture. Uses the Meshtastic Python client as a wire oracle."""
import argparse
import base64
import hashlib
import json
import re
from collections import deque
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlsplit


def read_page():
    header = Path(__file__).resolve().parents[2] / "src/network/browser/MessagingPage.h"
    source = header.read_text()
    page = re.search(r'R"ZTHTML\((.*?)\)ZTHTML"', source, re.S).group(1)
    policy = re.search(r'MessagingCsp\[\] = "([^"]+)"', source).group(1)
    for tag in ("style", "script"):
        block = re.search(f"<{tag}>(.*?)</{tag}>", page, re.S).group(1)
        digest = base64.b64encode(hashlib.sha256(block.encode()).digest()).decode()
        assert f"{tag}-src 'sha256-{digest}'" in policy
    return page.encode(), policy


def message_types(descriptors):
    if descriptors is None:
        from meshtastic.protobuf.mesh_pb2 import ToRadio, FromRadio
        return ToRadio, FromRadio
    from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
    pool = descriptor_pool.DescriptorPool()
    for definition in descriptor_pb2.FileDescriptorSet.FromString(descriptors.read_bytes()).file:
        pool.Add(definition)
    return tuple(message_factory.GetMessageClass(pool.FindMessageTypeByName("meshtastic.protobuf." + name))
                 for name in ("ToRadio", "FromRadio"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--descriptors", type=Path, help="Optional exported client FileDescriptorSet")
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--port-file", type=Path, required=True)
    args = parser.parse_args()
    to_radio, from_radio = message_types(args.descriptors)
    page, policy = read_page()
    pending = deque()
    observed = []
    received = '<b>Signal</b> Hello from the mesh'

    class Fixture(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, body=b"", mime="application/x-protobuf", cookie=False):
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Security-Policy", policy)
            if cookie:
                self.send_header("Set-Cookie", "zt_fixture=local; HttpOnly; SameSite=Strict; Path=/")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/":
                return self.reply(page, "text/html; charset=utf-8")
            if self.path != "/api/v1/fromradio":
                return self.send_error(404)
            if self.headers.get("Cookie") != "zt_fixture=local":
                return self.send_error(403)
            self.reply(pending.popleft().SerializeToString() if pending else b"")

        def do_PUT(self):
            if (self.path != "/api/v1/toradio" or self.headers.get_content_type() != "application/x-protobuf"
                    or urlsplit(self.headers.get("Origin", "")).netloc != self.headers.get("Host")):
                return self.send_error(403)
            length = int(self.headers.get("Content-Length", 0))
            if not 0 < length <= 512:
                return self.send_error(400)
            body = self.rfile.read(length)
            request = to_radio.FromString(body)
            if request.WhichOneof("payload_variant") == "want_config_id":
                pending.clear()
                identity = from_radio()
                identity.my_info.my_node_num = 0xdecafbad
                pending.append(identity)
                for index, name, role in ((0, "Local", 1), (3, "Trail", 2)):
                    frame = from_radio()
                    frame.channel.index, frame.channel.settings.name, frame.channel.role = index, name, role
                    pending.append(frame)
                pending.append(from_radio(config_complete_id=request.want_config_id))
                frame = from_radio()
                setattr(frame.packet, "from", 0x12345678)
                frame.packet.to, frame.packet.id, frame.packet.channel = 0xffffffff, 17, 3
                frame.packet.decoded.portnum, frame.packet.decoded.payload = 1, received.encode()
                pending.extend((frame, frame))
                return self.reply(body, cookie=True)
            if self.headers.get("Cookie") != "zt_fixture=local" or request.WhichOneof("payload_variant") != "packet":
                return self.send_error(403)
            packet = request.packet
            if not packet.id or not packet.to or packet.WhichOneof("payload_variant") != "decoded" or packet.decoded.portnum != 1:
                return self.send_error(400)
            observed.append(dict(id=packet.id, channel=packet.channel, to=packet.to, text=packet.decoded.payload.decode()))
            ack = from_radio()
            ack.packet.decoded.portnum, ack.packet.decoded.request_id = 5, packet.id
            ack.packet.decoded.payload = b"\x18\x00"
            pending.append(ack)
            echo = from_radio()
            echo.packet.CopyFrom(packet)
            setattr(echo.packet, "from", 0xdecafbad)
            pending.append(echo)
            args.result.write_text(json.dumps(observed, ensure_ascii=False))
            self.reply(body)

    with HTTPServer(("127.0.0.1", 0), Fixture) as server:
        args.port_file.write_text(str(server.server_port))
        server.serve_forever()


if __name__ == "__main__":
    main()
