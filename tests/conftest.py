import os
import socket
import subprocess
import time
import weakref
from pathlib import Path

import pytest

SPORED = os.environ.get("SPORED", str(Path(__file__).parents[1] / "build" / "spored"))


class Server:
    def __init__(self, *args, tmp_path=None):
        self.proc = subprocess.Popen(
            [SPORED, "--port", "0", *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        line = self.proc.stdout.readline().strip()
        if not line.startswith("spored listening on "):
            self.proc.kill()
            raise RuntimeError(f"spored failed: {line} {self.proc.stderr.read()}")
        self.addr = line.rsplit(" ", 1)[1]
        if self.addr.startswith("unix:"):
            self.unix = self.addr[5:]
            self.port = None
        else:
            self.unix = None
            self.port = int(self.addr.rsplit(":", 1)[1])

    def connect(self, timeout=5.0):
        if self.unix:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(self.unix)
        else:
            s = socket.create_connection(("127.0.0.1", self.port))
        s.settimeout(timeout)
        return s

    def stop(self):
        self.proc.terminate()
        try:
            rc = self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise
        return rc


@pytest.fixture
def spawn():
    servers = []

    def make(*args):
        s = Server(*args)
        servers.append(s)
        return s

    yield make
    for s in servers:
        if s.proc.poll() is None:
            assert s.stop() == 0


@pytest.fixture
def srv(spawn):
    return spawn()


_leftover = weakref.WeakKeyDictionary()  # bytes read past a response


def read_response(sock, head_only=False):
    """Read one HTTP/1.1 response; returns (status, headers, body).

    Bytes belonging to later responses are kept for the next call, so
    pipelined responses can be read one at a time."""
    buf = _leftover.pop(sock, b"")

    def more():
        nonlocal buf
        chunk = sock.recv(65536)
        if not chunk:
            raise EOFError(buf)
        buf += chunk

    while b"\r\n\r\n" not in buf:
        more()
    head, buf = buf.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ")[1])
    headers = {}
    for line in lines[1:]:
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    body = b""
    if head_only or status in (100, 204, 304):
        pass
    elif "content-length" in headers:
        n = int(headers["content-length"])
        while len(buf) < n:
            more()
        body, buf = buf[:n], buf[n:]
    elif headers.get("transfer-encoding") == "chunked":
        while True:
            while b"\r\n" not in buf:
                more()
            size_line, buf = buf.split(b"\r\n", 1)
            size = int(size_line, 16)
            while len(buf) < size + 2:
                more()
            body += buf[:size]
            buf = buf[size + 2 :]
            if size == 0:
                break
    else:  # close-delimited
        try:
            while True:
                more()
        except EOFError:
            body, buf = buf, b""
    if buf:
        _leftover[sock] = buf
    return status, headers, body


def request(srv, raw, **kw):
    with srv.connect() as s:
        s.sendall(raw)
        return read_response(s, **kw)


def wait_for(pred, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.02)
    return False
