#!/usr/bin/env python3
import contextlib
import os
import signal
import socket
import struct
import subprocess
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOCKS_CONF = ROOT / "socks.conf"


def free_port():
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def socks4_request(command, port, ip="127.0.0.1", user_id=b""):
    return (
        struct.pack("!BBH", 4, command, port)
        + socket.inet_aton(ip)
        + user_id
        + b"\0"
    )


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("socket closed before enough data arrived")
        data += chunk
    return data


class EchoServer:
    def __init__(self):
        self.port = free_port()
        self.pid = os.fork()
        if self.pid == 0:
            self._serve()
            os._exit(0)

    def _serve(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", self.port))
            server.listen(1)
            conn, _ = server.accept()
            with conn:
                while True:
                    data = conn.recv(4096)
                    if not data:
                        return
                    conn.sendall(data.upper())

    def close(self):
        try:
            os.kill(self.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


class SplitPromptShellServer:
    def __init__(self):
        self.port = free_port()
        self.pid = os.fork()
        if self.pid == 0:
            self._serve()
            os._exit(0)

    def _serve(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", self.port))
            server.listen(1)
            conn, _ = server.accept()
            with conn:
                conn.sendall(b"%")
                time.sleep(0.1)
                conn.sendall(b" ")
                command = b""
                while not command.endswith(b"\n"):
                    data = conn.recv(4096)
                    if not data:
                        return
                    command += data
                conn.sendall(b"sync-ok\n% ")

    def close(self):
        try:
            os.kill(self.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


class SocksServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original_conf = SOCKS_CONF.read_text() if SOCKS_CONF.exists() else None
        cls.test_case_dir = ROOT / "test_case"
        cls.had_test_case_dir = cls.test_case_dir.exists()
        cls.test_case_dir.mkdir(exist_ok=True)
        cls.sync_case = cls.test_case_dir / "codex_socks_sync.txt"
        cls.sync_case.write_text("echo sync\n")
        cls.socks_port = free_port()
        cls.server = subprocess.Popen(
            [str(ROOT / "socks_server"), str(cls.socks_port)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            preexec_fn=os.setsid,
        )
        time.sleep(0.2)

    @classmethod
    def tearDownClass(cls):
        os.killpg(os.getpgid(cls.server.pid), signal.SIGTERM)
        try:
            cls.server.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(cls.server.pid), signal.SIGKILL)
        if cls.original_conf is None:
            with contextlib.suppress(FileNotFoundError):
                SOCKS_CONF.unlink()
        else:
            SOCKS_CONF.write_text(cls.original_conf)
        with contextlib.suppress(FileNotFoundError):
            cls.sync_case.unlink()
        if not cls.had_test_case_dir:
            with contextlib.suppress(OSError):
                cls.test_case_dir.rmdir()

    def setUp(self):
        SOCKS_CONF.write_text("permit c *.*.*.*\npermit b *.*.*.*\n")

    def connect_to_socks(self):
        sock = socket.create_connection(("127.0.0.1", self.socks_port), timeout=2)
        sock.settimeout(2)
        return sock

    def test_connect_relays_traffic(self):
        echo = EchoServer()
        try:
            with self.connect_to_socks() as client:
                client.sendall(socks4_request(1, echo.port))
                reply = recv_exact(client, 8)
                self.assertEqual(reply[1], 90)
                client.sendall(b"connect-ok")
                self.assertEqual(recv_exact(client, len(b"CONNECT-OK")), b"CONNECT-OK")
        finally:
            echo.close()

    def test_firewall_reload_without_restarting_server(self):
        echo = EchoServer()
        try:
            SOCKS_CONF.write_text("permit c 10.*.*.*\npermit b *.*.*.*\n")
            with self.connect_to_socks() as client:
                client.sendall(socks4_request(1, echo.port))
                self.assertEqual(recv_exact(client, 8)[1], 91)

            SOCKS_CONF.write_text("permit c 127.0.0.1\npermit b *.*.*.*\n")
            with self.connect_to_socks() as client:
                client.sendall(socks4_request(1, echo.port))
                self.assertEqual(recv_exact(client, 8)[1], 90)
        finally:
            echo.close()

    def test_bind_relays_after_second_reply(self):
        with self.connect_to_socks() as client:
            client.sendall(socks4_request(2, 20))
            first_reply = recv_exact(client, 8)
            self.assertEqual(first_reply[1], 90)
            bind_port = struct.unpack("!H", first_reply[2:4])[0]

            peer = socket.create_connection(("127.0.0.1", bind_port), timeout=2)
            with peer:
                peer.settimeout(2)
                second_reply = recv_exact(client, 8)
                self.assertEqual(second_reply[1], 90)

                client.sendall(b"bind-client")
                self.assertEqual(recv_exact(peer, len(b"bind-client")), b"bind-client")
                peer.sendall(b"bind-peer")
                self.assertEqual(recv_exact(client, len(b"bind-peer")), b"bind-peer")

    def test_pj4_cgi_uses_socks_and_handles_split_prompt(self):
        shell = SplitPromptShellServer()
        try:
            env = os.environ.copy()
            env["QUERY_STRING"] = (
                f"h0=127.0.0.1&p0={shell.port}&f0={self.sync_case.name}"
                f"&sh=127.0.0.1&sp={self.socks_port}"
            )
            result = subprocess.run(
                [str(ROOT / "pj4.cgi")],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=3,
                check=True,
            )
            self.assertIn("sync-ok", result.stdout)
            self.assertIn("echo sync", result.stdout)
        finally:
            shell.close()


if __name__ == "__main__":
    unittest.main()
