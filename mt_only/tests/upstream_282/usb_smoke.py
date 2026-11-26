#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run meshTestic configuration and directed-text checks on an explicitly selected radio.

Based on meshtastic/meshTestic test/config.spec.ts at dcac7e5673005f4d8a2b1f0f6e06877b689d7519.
"""
import argparse
import queue
import re

from meshtastic.protobuf import mesh_pb2, portnums_pb2


def check_config(client, expected_node):
    if client.myInfo is None or client.myInfo.my_node_num != expected_node:
        raise ValueError("The selected device did not report the expected node")
    if not (client.metadata and client.localNode.localConfig.HasField("device")
            and client.nodesByNum and client.localNode.channels):
        raise ValueError("Missing device settings, metadata, node list or channels")


def check_ack(packet, sender, peer, request_id):
    decoded = packet.decoded
    routing = mesh_pb2.Routing.FromString(decoded.payload)
    if (getattr(packet, "from") != peer or packet.to != sender or
            decoded.portnum != portnums_pb2.ROUTING_APP or decoded.request_id != request_id or
            routing.WhichOneof("variant") != "error_reason" or routing.error_reason != mesh_pb2.Routing.NONE):
        raise ValueError("The peer did not return a successful routing ACK for this direct message")


def node_id(value):
    if not re.fullmatch(r"!?[0-9a-fA-F]{8}", value):
        raise argparse.ArgumentTypeError("Use the eight-digit hexadecimal node ID")
    number = int(value.lstrip("!"), 16)
    if not 1 < number < 0xfffffffe:
        raise argparse.ArgumentTypeError("A unicast node ID is required")
    return number


def main():
    from meshtastic.serial_interface import SerialInterface

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB serial device already assigned to this test")
    parser.add_argument("--expect-node", required=True, type=node_id)
    parser.add_argument("--send-to", type=node_id, help="Send one test text to this peer and wait for its ACK")
    args = parser.parse_args()
    if args.send_to == args.expect_node:
        parser.error("Choose a second node for the direct-message test")
    with SerialInterface(devPath=args.port, timeout=30) as client:
        check_config(client, args.expect_node)
        print("PASS USB configuration")
        if args.send_to is not None:
            replies = queue.Queue()

            # The pinned stock client dispatches an ordinary ACK to a sendText
            # callback when it has this name.
            def onAckNak(received):
                replies.put(received["raw"])

            sent = client.sendText("Zephtastic compatibility check", destinationId=args.send_to,
                                   wantAck=True, onResponse=onAckNak)
            try:
                answer = replies.get(timeout=60)
            except queue.Empty:
                raise SystemExit("Timed out waiting for the peer ACK")
            check_ack(answer, args.expect_node, args.send_to, sent.id)
            print("PASS peer ACK for directed text")


if __name__ == "__main__":
    main()
