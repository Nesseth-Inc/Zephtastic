#!/usr/bin/env python3
"""Exercise the actual browser FIN wait on loopback, plus Linux TIME_WAIT accounting."""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent


def time_wait(port):
    path = pathlib.Path('/proc/net/tcp')
    if not path.is_file():
        return None
    return sum(1 for line in path.read_text().splitlines()[1:]
               if int(line.split()[1].split(':')[1], 16) == port and line.split()[3] == '06')


def series(binary, mode):
    count = 25
    process = subprocess.Popen([str(binary), mode, str(count)], stdout=subprocess.PIPE, text=True)
    try:
        port = int(process.stdout.readline())
        server_closed = 0
        for index in range(count):
            with socket.create_connection(('127.0.0.1', port), timeout=3) as peer:
                peer.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n')
                response = b''
                while not response.endswith(b'\r\n\r\nok'):
                    chunk = peer.recv(512)
                    assert chunk and len(response) < 512, 'incomplete reply'
                    response += chunk
                if index == count-1:
                    # A peer that requires EOF still gets it after the bounded FIN wait.
                    assert peer.recv(1) == b''
                    continue
                # Let the close packet arrive. A length-aware client releases its socket
                # on its own even if the server is still waiting for that client FIN.
                peer.settimeout(.015)
                try:
                    assert peer.recv(1) == b''
                    server_closed += 1
                except socket.timeout:
                    pass
        assert process.wait(timeout=2) == 0
        held = time_wait(port)
        if mode == 'active':
            assert server_closed >= count-2, server_closed
            assert held is None or held >= count-2, held
        else:
            assert server_closed == 0, server_closed
            assert held is None or held <= 1, held
        return {'requests': count, 'server_closed_first_among_24_length_aware': server_closed,
                'server_time_wait_including_one_eof_client': held}
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as dest:
        binary = pathlib.Path(dest) / 'loopback'
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(HERE/'close_loopback.cpp'),
                        '-o',str(binary)],check=True)
        print(json.dumps({mode: series(binary,mode) for mode in ('active','passive')},sort_keys=True))
