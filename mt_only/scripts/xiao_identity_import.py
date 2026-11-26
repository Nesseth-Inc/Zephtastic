#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Confirm a stock Android identity restore on the physically attached ordinary ZT XIAO."""
import argparse
import base64
import binascii
import contextlib
import errno
import os
from pathlib import Path
import re
import secrets
import select
import struct
import sys
import time
from dataclasses import dataclass
from typing import Optional

from google.protobuf.message import DecodeError
from meshtastic.protobuf import mesh_pb2, portnums_pb2

HEADER = b"\x94\xc3"
PREFIX = b"zt_pki_import_v1 "


def public_key(text):
    """Accept the public key shown by Android, in base64 or hex."""
    try:
        result = bytes.fromhex(text) if re.fullmatch(r"[a-fA-F0-9]{64}", text) else base64.b64decode(text, validate=True)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("Expected a 32-byte public key in base64 or hex.") from exc
    if len(result) != 32:
        raise argparse.ArgumentTypeError("Expected a 32-byte public key.")
    return result


class PosixUsbPort:
    def __init__(self, fd):
        self.fd = fd
        self.physical_id = None
        if sys.platform.startswith("linux"):
            number = os.fstat(fd).st_rdev
            device = Path(f"/sys/dev/char/{os.major(number)}:{os.minor(number)}/device").resolve()
            for parent in device.parents:
                try:
                    vendor, product, serial = ((parent / name).read_text().strip()
                                               for name in ("idVendor", "idProduct", "serial"))
                except OSError:
                    continue
                if vendor and product and serial:
                    self.physical_id = (vendor, product, serial)
                    break

    def read(self, count):
        if not select.select([self.fd], [], [], 0.15)[0]:
            return b""
        try:
            data = os.read(self.fd, count)
        except BlockingIOError:
            return b""
        if not data:
            raise OSError("USB device disconnected")
        return data

    def write(self, data):
        pending = memoryview(data)
        deadline = time.monotonic() + 2
        while pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise TimeoutError("USB write timed out")
            try:
                count = os.write(self.fd, pending)
            except BlockingIOError:
                continue
            if not count:
                raise OSError("USB device disconnected")
            pending = pending[count:]
        return len(data)


@contextlib.contextmanager
def open_usb(port):
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        raise OSError("The XIAO USB identity helper supports Linux and macOS.")
    import fcntl
    import termios
    import tty

    def restore(operation, *args):
        try:
            operation(*args)
        except (OSError, termios.error) as exc:
            # A confirmed import schedules a device reboot. Its tty can already
            # be gone before the host returns from processing the confirmation.
            if not exc.args or exc.args[0] not in (errno.EIO, errno.ENODEV, errno.ENXIO):
                raise

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        with contextlib.ExitStack() as cleanup:
            cleanup.callback(os.close, fd)
            if not os.isatty(fd):
                raise OSError("Expected a physical USB serial device")
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.ioctl(fd, termios.TIOCEXCL)
            cleanup.callback(restore, fcntl.ioctl, fd, termios.TIOCNXCL)
            previous = termios.tcgetattr(fd)
            cleanup.callback(restore, termios.tcsetattr, fd, termios.TCSANOW, previous)
            # The S3's USB Serial/JTAG interprets modem-line transitions as
            # reset requests. Configure byte framing only and retain DTR/RTS.
            tty.setraw(fd, termios.TCSANOW)
            attributes = termios.tcgetattr(fd)
            attributes[4] = attributes[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, attributes)
            yield PosixUsbPort(fd)
    except termios.error as exc:
        raise OSError(*exc.args) from exc


@dataclass
class LocalIdentity:
    node: int
    boot: int
    silicon: bytes
    usb_id: Optional[tuple[str, str, str]]
    local_key: Optional[bytes] = None
    security_key: Optional[bytes] = None


class UsbSession:
    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()
        self.node = 0

    def send(self, message):
        data = message.SerializeToString(deterministic=True)
        frame = HEADER + struct.pack(">H", len(data)) + data
        if self.port.write(frame) != len(frame):
            raise OSError("Incomplete USB write")

    def receive(self, deadline):
        while time.monotonic() < deadline:
            offset = self.buffer.find(HEADER)
            if offset < 0:
                self.buffer[:] = self.buffer[-1:] if self.buffer.endswith(HEADER[:1]) else b""
            else:
                del self.buffer[:offset]
                if len(self.buffer) >= 4:
                    size = int.from_bytes(self.buffer[2:4], "big")
                    if not 0 < size <= 512:
                        del self.buffer[:2]
                        continue
                    if len(self.buffer) >= 4 + size:
                        data = bytes(self.buffer[4:4 + size])
                        del self.buffer[:4 + size]
                        try:
                            return mesh_pb2.FromRadio.FromString(data)
                        except DecodeError as exc:
                            raise ValueError("Invalid PhoneAPI frame from the USB port.") from exc
            self.buffer.extend(self.port.read(1024))
        raise TimeoutError("No matching USB reply")

    def handshake(self, *, allow_recovery=False, timeout=15):
        config_id = secrets.randbelow(0x7fffffff) + 1
        while config_id in (69420, 69421):  # reserved partial configuration handshakes
            config_id = secrets.randbelow(0x7fffffff) + 1
        self.buffer.clear()
        self.node = 0
        snapshot = None
        self.send(mesh_pb2.ToRadio(want_config_id=config_id))
        deadline = time.monotonic() + timeout
        while True:
            msg = self.receive(deadline)
            if msg.HasField("my_info"):
                info = msg.my_info
                if info.pio_env != "mt-zephyr-xiao" and not (allow_recovery and not info.pio_env):
                    raise ValueError("The attached device is not an ordinary ZT XIAO.")
                snapshot = LocalIdentity(info.my_node_num, info.reboot_count,
                                         bytes(info.device_id), getattr(self.port, "physical_id", None))
            elif snapshot and msg.HasField("node_info") and snapshot.local_key is None:
                # ZT emits its own NodeInfo first, before its cached peer list.
                own = msg.node_info
                if own.num != snapshot.node or own.user.id != f"!{snapshot.node:08x}":
                    raise ValueError("Unexpected local NodeInfo in the XIAO handshake.")
                snapshot.local_key = bytes(own.user.public_key)
            elif snapshot and msg.HasField("config") and msg.config.HasField("security"):
                snapshot.security_key = bytes(msg.config.security.public_key)
            if msg.config_complete_id == config_id:
                if snapshot is None or not 1 < snapshot.node < 0xfffffffe:
                    raise ValueError("No valid local ZT node in handshake.")
                self.node = snapshot.node
                return snapshot

    def command(self, command, timeout=4):
        tx = mesh_pb2.ToRadio()
        packet = tx.packet
        packet.id = secrets.randbelow(0xffffffff) + 1
        packet.to = self.node
        packet.decoded.portnum = portnums_pb2.TEXT_MESSAGE_APP
        packet.decoded.payload = PREFIX + command
        self.send(tx)
        deadline = time.monotonic() + timeout
        while True:
            reply = self.receive(deadline)
            if not reply.HasField("packet"):
                continue
            p = reply.packet
            if getattr(p, "from") != self.node or p.to != self.node or p.decoded.request_id != packet.id:
                continue
            if p.decoded.portnum == portnums_pb2.ROUTING_APP:
                raise ValueError("ZT refused the local request. Check the installation and USB session.")
            if p.decoded.portnum == portnums_pb2.TEXT_MESSAGE_APP:
                return p.decoded.payload


def verify_reboot(session, before, expected, reconnect=None):
    deadline = time.monotonic() + 45
    same_fd = True
    silicon_known = (len(before.silicon) == 16 and
                     before.silicon not in (bytes(16), bytes([255]) * 16))
    while time.monotonic() < deadline:
        # Reboot is scheduled after commit. A fresh handshake is read-only.
        time.sleep(1)
        try:
            if session is None:
                if reconnect is None:
                    break
                session = reconnect()
                if before.usb_id is not None and getattr(session.port, "physical_id", None) != before.usb_id:
                    raise ValueError("The USB port now belongs to a different device.")
            after = session.handshake(timeout=min(6, max(0, deadline - time.monotonic())))
        except TimeoutError:
            if reconnect is not None and (silicon_known or before.usb_id is not None):
                same_fd = False
                session = None
            continue
        except OSError:
            same_fd = False
            session = None
            continue

        if ((silicon_known and after.silicon != before.silicon) or
                (before.usb_id is not None and after.usb_id != before.usb_id) or
                (not same_fd and not silicon_known and before.usb_id is None)):
            raise ValueError("The reconnected USB board could not be matched to the original XIAO.")
        if after.boot == before.boot:
            continue
        if (after.boot <= before.boot or after.node != binascii.crc32(expected) or
                after.local_key != expected or after.security_key != expected):
            raise ValueError("The XIAO rebooted without the expected local public identity.")
        return
    raise TimeoutError("Could not verify the XIAO’s identity after reboot.")


def run(session, expected, *, resume=False, reconnect=None):
    before = session.handshake(allow_recovery=resume)
    if not resume and (not before.boot or before.node == binascii.crc32(expected) or
                       before.local_key != b"" or before.security_key != b""):
        raise ValueError("The ordinary XIAO must report an unenrolled local identity before restore.")
    if resume and session.command(b"capabilities") != b"pki_import_v1 profile=mt-zephyr-xiao held=1":
        raise ValueError("No interrupted stock import was identified on this XIAO.")
    armed = re.fullmatch(rb"pki_import_v1 rc=0 armed=1 challenge=([0-9a-f]{32})", session.command(b"arm"))
    if not armed:
        raise ValueError("ZT did not open an identity restore window.")
    commit_attempted = False
    try:
        if resume:
            node = f"{binascii.crc32(expected):08x}".encode()
            commit_attempted = True
            try:
                result = session.command(b"resume " + node + b" " + expected.hex().encode() + b" " + armed[1],
                                         timeout=20)
            except OSError as exc:
                raise OSError("Resume receipt unavailable. Verify the local public identity after boot.") from exc
            if result != b"pki_import_v1 rc=0 committed=1 reboot=1":
                raise ValueError("Resume was not confirmed. Check the radio's identity before retrying.")
            print(f"ZT completed the interrupted import for !{node.decode()}. Wait for reboot and verify in Android.")
            return
        prompted = False
        deadline = time.monotonic() + 85
        while time.monotonic() < deadline:
            status = session.command(b"status")
            match = re.fullmatch(
                rb"pki_import_v1 rc=0 armed=1 staged=([01]) node=([0-9a-f]{8})"
                rb" pub=([0-9a-f]{64}|) ble_eligible=([01])", status)
            if not match:
                raise ValueError("Unexpected identity import status from this firmware.")
            staged, node, key, eligible = match.groups()
            if eligible != b"1":
                raise ValueError("Connect Android using the private fixed PIN before running this helper.")
            if staged == b"0":
                if node != b"00000000" or key:
                    raise ValueError("Inconsistent empty identity status.")
                if not prompted:
                    print("On the authenticated Android connection, use Security → Restore Keys now.", flush=True)
                    prompted = True
                time.sleep(0.6)
                continue
            if key != expected.hex().encode() or int(node, 16) != binascii.crc32(expected):
                raise ValueError("The staged identity differs from the public key supplied before import.")
            commit_attempted = True
            try:
                result = session.command(b"confirm " + node + b" " + key + b" " + armed[1], timeout=20)
            except OSError:
                result = None  # The radio may have committed before the transport reset.
            if result is not None and result != b"pki_import_v1 rc=0 committed=1 reboot=1":
                raise ValueError("Unexpected commit receipt. Verify the local public identity after boot.")
            try:
                verify_reboot(session, before, expected, reconnect)
            except (OSError, ValueError) as exc:
                receipt = "ZT acknowledged the commit." if result is not None else "No commit receipt arrived."
                raise ValueError(f"{receipt} {exc} Check the attached radio before taking further action.") from exc
            receipt = "Commit receipt received." if result is not None else "Confirmed from reboot readback."
            print(f"ZT committed node !{node.decode()}. Full local public key verified after reboot. {receipt}")
            return
        raise TimeoutError("The local import window expired.")
    finally:
        if not commit_attempted:
            try:
                session.command(b"cancel")
            except (OSError, ValueError, TimeoutError):
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Physical serial port for the factory-flashed XIAO")
    parser.add_argument("--public-key", required=True, type=public_key, help="Expected public key recorded on stock firmware")
    parser.add_argument("--resume", action="store_true",
                        help="Complete a previously USB-confirmed import interrupted during its storage writes")
    args = parser.parse_args()
    with contextlib.ExitStack() as connection:
        def connect():
            connection.close()
            return UsbSession(connection.enter_context(open_usb(args.port)))
        run(connect(), args.public_key, resume=args.resume, reconnect=connect)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TimeoutError, KeyboardInterrupt) as exc:
        sys.exit(str(exc) or "Identity restore interrupted.")
