#!/usr/bin/env python3
"""Run the production net_test on macOS with loopback HTTP/TCP endpoints.

This validates command exit status and the process deadline. It does not exercise
the device's DNS, routing, TLS trust store, Wi-Fi driver or Apple services.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HTTPHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/stall':
            self.server.stop.wait(5)
            return
        self.send_response(503 if self.path == '/failure' else 200)
        self.send_header('Content-Length', '2')
        self.end_headers()
        self.wfile.write(b'ok')

    def log_message(self, *_args):
        pass


class EmptyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.recv(4096)


class StallHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.recv(4096)
        self.server.stop.wait(5)


class ResetHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.recv(4096)
        self.request.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        self.request.close()


class ThreadedTCPServer(socketserver.ThreadingTCPServer):
    daemon_threads = True


class NetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='net-test-')
        directory = Path(cls.temporary.name)
        cls.binary = directory / 'net_test'
        compatibility = directory / 'host.h'
        # macOS declares the iOS-only WWAN flag unavailable; reach is not tested.
        compatibility.write_text('#include <SystemConfiguration/SCNetworkReachability.h>\n'
                                 '#define kSCNetworkReachabilityFlagsIsWWAN 0\n')
        subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-fobjc-arc', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-deprecated-declarations', '-include', str(compatibility),
                        '-framework', 'Foundation', '-framework', 'SystemConfiguration',
                        str(ROOT / 'userland/src/tests/net_test.m'), '-o', str(cls.binary)], check=True)
        cls.http = ThreadingHTTPServer(('127.0.0.1', 0), HTTPHandler)
        cls.empty = ThreadedTCPServer(('127.0.0.1', 0), EmptyHandler)
        cls.stall = ThreadedTCPServer(('127.0.0.1', 0), StallHandler)
        cls.reset = ThreadedTCPServer(('127.0.0.1', 0), ResetHandler)
        cls.servers = [cls.http, cls.empty, cls.stall, cls.reset]
        for server in cls.servers:
            server.stop = threading.Event()
            threading.Thread(target=server.serve_forever, daemon=True).start()
        cls.base = f'http://127.0.0.1:{cls.http.server_address[1]}'

    @classmethod
    def tearDownClass(cls):
        for server in cls.servers:
            server.stop.set()
            server.shutdown()
            server.server_close()
        cls.temporary.cleanup()

    def run_probe(self, *arguments, status=0, text=None):
        result = subprocess.run([str(self.binary), *map(str, arguments)], capture_output=True,
                                text=True, timeout=6)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        if text is not None:
            self.assertIn(text, result.stdout + result.stderr)
        return result

    def test_resolve(self):
        self.run_probe('resolve', '127.0.0.1')
        self.run_probe('resolve', 'invalid host', status=1)

    def test_fetch_status(self):
        self.run_probe('fetch', self.base, text='status 200')
        self.run_probe('fetch', self.base + '/failure', status=1, text='status 503')

    def test_fetch_invalid_url(self):
        for address in ['not-a-url', 'file:///etc/hosts', 'http://']:
            with self.subTest(address=address):
                self.run_probe('fetch', address, status=1, text='invalid HTTP(S) URL')

    def test_fetch_transport_failure(self):
        self.run_probe('fetch', f'http://127.0.0.1:{self.empty.server_address[1]}',
                       status=1, text='FAILED')

    def test_tcp(self):
        self.run_probe('tcp', '127.0.0.1', self.http.server_address[1], text='HTTP/1.0 200')
        self.run_probe('tcp', '127.0.0.1', self.empty.server_address[1], status=1, text='0 bytes')
        self.run_probe('tcp', 'invalid host', '80', status=1)
        self.run_probe('tcp', '127.0.0.1', self.reset.server_address[1], status=1, text='read failed')

    def test_waitaddr(self):
        self.run_probe('waitaddr', 'lo0', '0', text='an address')
        self.run_probe('waitaddr', 'net_test_missing', '0', status=1, text='NO address')

    def test_failure_is_retained_and_later_steps_run(self):
        self.run_probe('fetch', self.base + '/failure', 'fetch', self.base,
                       status=1, text='status 200')
        self.run_probe('resolve', 'invalid host', 'resolve', '127.0.0.1', status=1,
                       text='resolve 127.0.0.1')

    def test_deadline_validation(self):
        for value in ['0', '-1', '1.5', '1x', '4294967296', '9999999999999999999999']:
            with self.subTest(value=value):
                self.run_probe('--deadline', value, 'show', status=1)
        self.run_probe('--deadline', '1', status=1)
        self.run_probe('--deadline', '3', 'resolve', '127.0.0.1')

    def test_deadline_bounds_blocked_operations(self):
        commands = [('fetch', self.base + '/stall'),
                    ('tcp', '127.0.0.1', self.stall.server_address[1]), ('wait', '5')]
        for command in commands:
            with self.subTest(command=command):
                start = time.monotonic()
                self.run_probe('--deadline', '1', *command, status=124, text='deadline expired')
                self.assertLess(time.monotonic() - start, 3)


if __name__ == '__main__':
    unittest.main(verbosity=2)
