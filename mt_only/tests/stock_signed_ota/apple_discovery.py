#!/usr/bin/env python3
"""Optional Apple Network.framework regression. Pass a local Meshtastic-Apple checkout."""
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

APPLE = "4ea817379c4d70075c3c5b8cc0cecee7938a23bd"
FIRMWARE = "Meshtastic/Views/Settings/Firmware/ESP32 OTA/"
ANDROID_BEACON = b"Meshtastic_1234 1.0.1"
APPLE_BEACON = b"MeshtasticOTA"
WRAPPER = """
import Foundation
final class UIApplication { static let shared=UIApplication();var isIdleTimerDisabled=false }
enum BLEOTAFailure: Error { case unexpectedResponse(String) }
struct Logger {
    static let services=Logger()
    func info(_ value: @autoclosure () -> String) {}
    func error(_ value: @autoclosure () -> String) {}
}
@main struct Test {
    @MainActor static func main() async {
        let model=ESP32WifiOTAViewModel()
        await model.startUpdate(firmwareUrl:URL(fileURLWithPath:CommandLine.arguments[1]))
        print(model.otaState == .completed && model.progress == 1 ? "PASS":"FAIL")
    }
}
"""


def compile_client(repo, work):
    def source(path):
        return subprocess.check_output(["git", "-C", repo, "show", APPLE + ":" + FIRMWARE + path]).decode()

    model = source("WiFi/ESP32WifiOTAViewModel.swift")
    # Constrain the stock listener to localhost. All decision, flow cancellation,
    # endpoint selection, upload and ACK handling code stays as shipped.
    changes = {
        "import UIKit\n": "",
        "parameters.allowLocalEndpointReuse = true":
            'parameters.allowLocalEndpointReuse = true\n'
            '        parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: port)\n'
            '        parameters.requiredInterfaceType = .loopback',
        "try NWListener(using: parameters, on: port)": "try NWListener(using: parameters)",
    }
    for old, new in changes.items():
        assert model.count(old) == 1
        model = model.replace(old, new)
    paths = [work / name for name in ("Model.swift", "Enums.swift", "Test.swift")]
    for path, text in zip(paths, (model, source("Helpers/OTAEnums.swift"), WRAPPER)):
        path.write_text(text)
    subprocess.run(["swiftc", "-swift-version", "5", "-O", *paths, "-o", work / "native"], check=True)


def accepts(work, order, separate_senders):
    address = "127.0.0.1"
    data = (work / "body").read_bytes()
    # The Apple broadcast receiver already has UDP 3232 on this loopback IP.
    # The simulated radio owns a different port for both TCP and its Apple UDP.
    with (socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as apple,
          socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as android, socket.socket() as tcp):
        apple.bind((address, 0))
        android.bind((address, 0))
        tcp.bind(apple.getsockname())
        tcp.listen(1)
        tcp.settimeout(5)
        done = threading.Event()

        def announce():
            for _ in range(8):
                if done.wait(0.45):
                    break
                for payload in order:
                    sender = android if separate_senders and payload == ANDROID_BEACON else apple
                    sender.sendto(payload, (address, 3232))

        with subprocess.Popen([work / "native", work / "body"], stdout=subprocess.PIPE) as client:
            thread = threading.Thread(target=announce)
            thread.start()
            try:
                try:
                    connection, _ = tcp.accept()
                except TimeoutError:
                    return False
                with connection:
                    connection.settimeout(3)
                    with connection.makefile("rb") as stream:
                        command = f"OTA {len(data)} {hashlib.sha256(data).hexdigest()}\n"
                        assert stream.readline(128) == command.encode()
                        connection.sendall(b"ERASING\nOK\n")
                        assert stream.read(len(data)) == data
                        connection.sendall(b"ACK\nOK\n")
                return client.communicate(timeout=5)[0].strip() == b"PASS"
            finally:
                done.set()
                thread.join()
                if client.poll() is None:
                    client.kill()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: apple_discovery.py PATH_TO_MESHTASTIC_APPLE")
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        compile_client(Path(sys.argv[1]).resolve(), work)
        (work / "body").write_bytes(b"loopback test only\n" * 130)
        cases = (("same_flow_android_first", (ANDROID_BEACON, APPLE_BEACON), False),
                 ("same_flow_apple_first", (APPLE_BEACON, ANDROID_BEACON), False),
                 ("distinct_flows_android_first", (ANDROID_BEACON, APPLE_BEACON), True),
                 ("distinct_flows_apple_first", (APPLE_BEACON, ANDROID_BEACON), True))
        actual = {name: accepts(work, order, separate) for name, order, separate in cases}
        print(json.dumps({"apple": APPLE, **actual}))
        # Platform versions which deliver both datagrams on a shared flow are
        # acceptable. Both orders on the supported distinct flows must work.
        assert actual["same_flow_apple_first"]
        assert actual["distinct_flows_android_first"] and actual["distinct_flows_apple_first"]


if __name__ == "__main__":
    main()
