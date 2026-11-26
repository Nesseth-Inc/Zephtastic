#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import base64
import binascii
import contextlib
import io
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from xiao_identity_import import HEADER, PREFIX, UsbSession, open_usb, mesh_pb2, portnums_pb2, public_key, run

# Public fixture and a dummy challenge for a synthetic USB session.
PUBLIC = bytes(range(32))
CHALLENGE = b"abcdef0123456789" * 2


class TestPort:
    def __init__(self, profile="mt-zephyr-xiao", mismatch=False, eligible=True, held=False):
        self.profile = profile
        self.mismatch = mismatch
        self.eligible = eligible
        self.held = held
        self.incoming = bytearray(b"boot output\n")
        self.commands = []
        self.committed = False
        self.handshakes = 0
        self.ack_lost = False
        self.disconnected = False
        self.boot_lag = False
        self.prior_key = b""
        self.post_node = binascii.crc32(PUBLIC)
        self.post_key = PUBLIC
        self.security_key = PUBLIC
        self.silicon = bytes(range(16))
        self.post_silicon = self.silicon
        self.post_boot = 4
        self.physical_id = ("303a", "1001", "synthetic-serial")

    def queue(self, msg):
        data = msg.SerializeToString(deterministic=True)
        self.incoming.extend(HEADER + struct.pack(">H", len(data)) + data)

    def read(self, count):
        if self.ack_lost and self.committed and not self.incoming:
            raise TimeoutError("Synthetic lost receipt")
        count = min(count, 5)
        data = bytes(self.incoming[:count])
        del self.incoming[:count]
        return data

    def write(self, frame):
        assert frame[:2] == HEADER and int.from_bytes(frame[2:4], "big") == len(frame) - 4
        msg = mesh_pb2.ToRadio.FromString(frame[4:])
        if msg.want_config_id:
            self.handshakes += 1
            if self.disconnected and self.committed:
                raise OSError("Synthetic disconnected tty")
            post = self.committed and not (self.boot_lag and self.handshakes == 2)
            info = mesh_pb2.FromRadio()
            info.my_info.my_node_num = self.post_node if post else 100
            info.my_info.pio_env = self.profile
            if not self.held:
                info.my_info.device_id = self.post_silicon if post else self.silicon
                info.my_info.reboot_count = self.post_boot if post else 3
            self.queue(info)
            if not self.held:
                own = mesh_pb2.FromRadio()
                own.node_info.num = info.my_info.my_node_num
                own.node_info.user.id = f"!{own.node_info.num:08x}"
                own.node_info.user.public_key = self.post_key if post else self.prior_key
                self.queue(own)
                security = mesh_pb2.FromRadio()
                security.config.security.public_key = self.security_key if post else self.prior_key
                self.queue(security)
                # A cached remote key can never substitute for either local field.
                own.node_info.num = 123
                own.node_info.user.id = "!0000007b"
                own.node_info.user.public_key = PUBLIC
                self.queue(own)
            self.queue(mesh_pb2.FromRadio(config_complete_id=msg.want_config_id))
            return len(frame)
        p = msg.packet
        assert p.to == 100 and not getattr(p, "from") and not p.hop_limit and not p.want_ack
        assert p.decoded.portnum == portnums_pb2.TEXT_MESSAGE_APP and p.decoded.payload.startswith(PREFIX)
        command = p.decoded.payload[len(PREFIX):]
        self.commands.append(command.split()[0])
        key = bytes(reversed(PUBLIC)) if self.mismatch else PUBLIC
        candidate = f"node={binascii.crc32(key):08x} pub={key.hex()}".encode()
        special = None
        if command == b"capabilities":
            special = b"pki_import_v1 profile=mt-zephyr-xiao held=" + (b"1" if self.held else b"0")
        elif command == b"arm":
            reply = b"armed=1 challenge=" + CHALLENGE
        elif command == b"status":
            if self.commands.count(b"status") == 1:
                reply = b"armed=1 staged=0 node=00000000 pub="
            else:
                reply = b"armed=1 staged=1 " + candidate
            reply += b" ble_eligible=" + (b"1" if self.eligible else b"0")
        elif command == b"cancel":
            reply = b"armed=0"
        else:
            assert command == (b"resume " if self.held else b"confirm ") + f"{binascii.crc32(key):08x} {key.hex()} ".encode() + CHALLENGE
            self.committed = True
            if self.ack_lost:
                return len(frame)
            reply = b"committed=1 reboot=1"
        response = mesh_pb2.FromRadio()
        setattr(response.packet, "from", 100)
        response.packet.to = 100
        response.packet.decoded.portnum = portnums_pb2.TEXT_MESSAGE_APP
        response.packet.decoded.request_id = p.id
        response.packet.decoded.payload = special or (b"pki_import_v1 rc=0 " + reply)
        self.queue(response)
        return len(frame)


class PortLifecycle(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith("linux") or sys.platform == "darwin", "POSIX tty")
    def test_raw_pty_preserves_modem_lines_and_restores_mode(self):
        import fcntl
        import pty
        import termios

        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        path = os.ttyname(slave)
        previous = termios.tcgetattr(slave)
        # Exercise the actual OS tty. Restrict explicit ioctls to reserving
        # and releasing it. No modem-control writes are allowed.
        with mock.patch.object(fcntl, "ioctl", wraps=fcntl.ioctl) as ioctl:
            with open_usb(path) as port:
                self.assertFalse(termios.tcgetattr(slave)[3] & termios.ICANON)
                self.assertEqual(termios.tcgetattr(slave)[4:6], [termios.B115200] * 2)
                self.assertEqual(port.write(b"\x94\xc3\x00\x02\x18\x7b"), 6)
                self.assertEqual(os.read(master, 6), b"\x94\xc3\x00\x02\x18\x7b")
                os.write(master, b"\x94\xc3\x00")
                self.assertEqual(port.read(1024), b"\x94\xc3\x00")
            self.assertEqual([call.args[1] for call in ioctl.call_args_list],
                             [termios.TIOCEXCL, termios.TIOCNXCL])
        restored = termios.tcgetattr(slave)
        if sys.platform == "darwin":
            # Darwin sets its readback-only pending-retype indication on
            # return to canonical mode, even for an empty PTY input queue.
            restored[3] &= ~termios.PENDIN
            previous[3] &= ~termios.PENDIN
        self.assertEqual(restored, previous)

    def test_unsupported_host_refuses_before_open(self):
        with mock.patch.object(sys, "platform", "win32"), mock.patch.object(os, "open") as opening:
            with self.assertRaisesRegex(OSError, "supports Linux and macOS"):
                with open_usb("COM8"):
                    self.fail("unexpected open")
        opening.assert_not_called()


class UsbConfirmation(unittest.TestCase):
    def setUp(self):
        pause = mock.patch("xiao_identity_import.time.sleep")
        pause.start()
        self.addCleanup(pause.stop)

    def test_expected_key_must_have_been_absent_before_arming(self):
        port = TestPort()
        port.prior_key = PUBLIC
        with self.assertRaisesRegex(ValueError, "unenrolled"):
            run(UsbSession(port), PUBLIC)
        self.assertEqual(port.commands, [])

    def test_lost_receipt_confirmed_once_by_new_boot_and_both_local_keys(self):
        port = TestPort()
        port.ack_lost = port.boot_lag = True
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            run(UsbSession(port), PUBLIC)
        self.assertEqual(port.commands, [b"arm", b"status", b"status", b"confirm"])
        self.assertEqual(port.handshakes, 3)
        self.assertIn("Confirmed from reboot readback", output.getvalue())

    def test_wrong_node_keys_or_board_fail_even_with_a_positive_receipt(self):
        for name, value in [("post_node", 101), ("post_key", b""), ("security_key", b""),
                            ("post_silicon", bytes(range(1,17))), ("post_boot", 2)]:
            with self.subTest(name=name):
                port = TestPort()
                setattr(port, name, value)
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, "acknowledged"):
                    run(UsbSession(port), PUBLIC)
                self.assertEqual(port.commands, [b"arm", b"status", b"status", b"confirm"])

    def test_disconnect_verifies_stable_reopened_board_without_repeating_command(self):
        port = TestPort()
        port.ack_lost = port.disconnected = True
        recovered = TestPort()
        recovered.committed = True
        with contextlib.redirect_stdout(io.StringIO()):
            run(UsbSession(port), PUBLIC, reconnect=lambda: UsbSession(recovered))
        self.assertEqual(recovered.commands, [])
        self.assertEqual(port.commands, [b"arm", b"status", b"status", b"confirm"])

    def test_lost_receipt_and_wrong_reopened_device_is_ambiguous(self):
        port = TestPort()
        port.ack_lost = port.disconnected = True
        recovered = TestPort()
        recovered.committed = True
        recovered.physical_id = ("303a", "1001", "different")
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, "No commit receipt"):
            run(UsbSession(port), PUBLIC, reconnect=lambda: UsbSession(recovered))
        self.assertEqual(recovered.commands, [])
        self.assertEqual(port.commands[-1], b"confirm")

    def test_reopened_unidentified_board_never_qualifies(self):
        port = TestPort()
        port.silicon = bytes(16)
        port.physical_id = None
        port.ack_lost = port.disconnected = True
        recovered = TestPort()
        recovered.committed = True
        recovered.physical_id = None
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, "could not be matched"):
            run(UsbSession(port), PUBLIC, reconnect=lambda: UsbSession(recovered))
        self.assertEqual(port.commands.count(b"confirm"), 1)

    def test_restricted_resume_receipt_loss_never_repeats_or_cancels(self):
        port = TestPort(profile="", held=True)
        port.ack_lost = True
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(OSError, "receipt unavailable"):
            run(UsbSession(port), PUBLIC, resume=True)
        self.assertEqual(port.commands, [b"capabilities", b"arm", b"resume"])

    def test_matching_independent_identity(self):
        port = TestPort()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            run(UsbSession(port), public_key(base64.b64encode(PUBLIC).decode()))
        self.assertEqual(port.commands, [b"arm", b"status", b"status", b"confirm"])
        self.assertNotIn(CHALLENGE.decode(), out.getvalue())

    def test_substituted_identity_cancels(self):
        port = TestPort(mismatch=True)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, "public key"):
            run(UsbSession(port), public_key(PUBLIC.hex()))
        self.assertEqual(port.commands, [b"arm", b"status", b"status", b"cancel"])

    def test_ineligible_link_never_prompts_to_send_keys(self):
        port = TestPort(eligible=False)
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaisesRegex(ValueError, "private fixed PIN"):
            run(UsbSession(port), PUBLIC)
        self.assertNotIn("Restore Keys", output.getvalue())
        self.assertEqual(port.commands, [b"arm", b"status", b"cancel"])

    def test_held_resume_requires_exact_capabilities_first(self):
        port = TestPort(profile="", held=True)
        with contextlib.redirect_stdout(io.StringIO()):
            run(UsbSession(port), PUBLIC, resume=True)
        self.assertEqual(port.commands, [b"capabilities", b"arm", b"resume"])
        unrecognized = TestPort(profile="")
        with self.assertRaisesRegex(ValueError, "No interrupted stock import"):
            run(UsbSession(unrecognized), PUBLIC, resume=True)
        self.assertEqual(unrecognized.commands, [b"capabilities"])

    def test_other_profiles_receive_no_commands(self):
        for profile in ["seeed-xiao-s3", "mt-zephyr-xiao-198", "mt-zephyr-t096"]:
            port = TestPort(profile=profile)
            with self.assertRaisesRegex(ValueError, "ordinary ZT XIAO"):
                run(UsbSession(port), PUBLIC)
            self.assertEqual(port.commands, [])


if __name__ == "__main__":
    unittest.main()
