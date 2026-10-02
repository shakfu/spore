"""WebSocket (RFC 6455): the `websockets` client as oracle, raw frames for
protocol violations."""

import base64
import contextlib
import json
import os
import socket
import struct
import threading
import time

import pytest

from conftest import read_response, requires

pytestmark = requires("ws")

websockets = pytest.importorskip("websockets.sync.client")
from websockets.exceptions import ConnectionClosed, ConnectionClosedOK  # noqa: E402


def url(srv, path):
    return f"ws://127.0.0.1:{srv.port}{path}"


# ---- raw client ----------------------------------------------------------


def handshake(srv, path="/ws/echo", key=None, extra=""):
    key = key or base64.b64encode(os.urandom(16)).decode()
    s = srv.connect()
    s.sendall(
        (
            f"GET {path} HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n{extra}\r\n"
        ).encode()
    )
    return s


def upgraded(srv, path="/ws/echo"):
    s = handshake(srv, path)
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    head, rest = buf.split(b"\r\n\r\n", 1)
    assert head.startswith(b"HTTP/1.1 101 ")
    return s, rest


def frame(op, payload=b"", fin=True, mask=True, rsv=0):
    b0 = (0x80 if fin else 0) | rsv | op
    n = len(payload)
    m = 0x80 if mask else 0
    if n < 126:
        h = struct.pack("!BB", b0, m | n)
    elif n <= 0xFFFF:
        h = struct.pack("!BBH", b0, m | 126, n)
    else:
        h = struct.pack("!BBQ", b0, m | 127, n)
    if not mask:
        return h + payload
    key = os.urandom(4)
    return h + key + bytes(b ^ key[i & 3] for i, b in enumerate(payload))


class Reader:
    """Parses unmasked server frames from a socket."""

    def __init__(self, sock, pending=b""):
        self.s, self.buf = sock, pending

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def frame(self):
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        assert not b1 & 0x80, "server frames must not be masked"
        n, off = b1 & 0x7F, 2
        if n == 126:
            self._need(4)
            n, off = struct.unpack("!H", self.buf[2:4])[0], 4
        elif n == 127:
            self._need(10)
            n, off = struct.unpack("!Q", self.buf[2:10])[0], 10
        self._need(off + n)
        payload, self.buf = self.buf[off : off + n], self.buf[off + n :]
        return b0 & 0x0F, bool(b0 & 0x80), payload

    def close_code(self):
        op, _, p = self.frame()
        assert op == 8
        return struct.unpack("!H", p[:2])[0] if len(p) >= 2 else None


def eof(sock):
    try:
        return sock.recv(1) == b""
    except (ConnectionResetError, socket.timeout):
        return False


# ---- handshake -----------------------------------------------------------


def test_accept_key_matches_rfc_example(srv):
    # RFC 6455 section 1.3 gives this key and its expected accept value.
    s = handshake(srv, key="dGhlIHNhbXBsZSBub25jZQ==")
    status, h, _ = read_response(s, head_only=True)
    assert status == 101
    assert h["sec-websocket-accept"] == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    assert h["upgrade"].lower() == "websocket" and "upgrade" in h["connection"].lower()
    s.close()


@pytest.mark.parametrize(
    "extra,key,status",
    [
        ("", "short", 400),
        ("", "dGhlIHNhbXBsZSBub25jZQ", 400),  # missing padding
    ],
)
def test_bad_keys_rejected(srv, extra, key, status):
    s = handshake(srv, key=key, extra=extra)
    assert read_response(s)[0] == status


def test_wrong_version_gets_426(srv):
    raw = (
        b"GET /ws/echo HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        b"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        b"Sec-WebSocket-Version: 8\r\n\r\n"
    )
    with srv.connect() as s:
        s.sendall(raw)
        status, h, _ = read_response(s)
    assert status == 426 and h["sec-websocket-version"] == "13"


def test_plain_get_on_ws_route_is_400(srv):
    with srv.connect() as s:
        s.sendall(b"GET /ws/echo HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert read_response(s)[0] == 400


def test_cross_site_upgrade_rejected(srv):
    # Browsers apply no CORS to WebSockets; the Origin check is the defence.
    s = handshake(srv, extra="Origin: https://evil.example\r\n")
    assert read_response(s)[0] == 403
    s = handshake(srv, extra="Origin: http://localhost:3000\r\n")
    assert read_response(s, head_only=True)[0] == 101


def test_token_applies_to_upgrade(spawn):
    srv = spawn("--token", "t")
    assert read_response(handshake(srv))[0] == 401
    s = handshake(srv, extra="Authorization: Bearer t\r\n")
    assert read_response(s, head_only=True)[0] == 101


def test_subprotocol_selected_only_when_offered(srv):
    with websockets.connect(url(srv, "/ws/echo"), subprotocols=["x", "echo"]) as ws:
        assert ws.subprotocol == "echo"
    with websockets.connect(url(srv, "/ws/echo")) as ws:
        assert ws.subprotocol is None


# ---- messages ------------------------------------------------------------


def test_echo_text_and_binary(srv):
    with websockets.connect(url(srv, "/ws/echo"), max_size=None) as ws:
        ws.send("héllo \U0001f600")
        assert ws.recv() == "héllo \U0001f600"
        for n in (0, 125, 126, 65535, 65536, 1 << 20):
            data = os.urandom(n)
            ws.send(data)
            assert ws.recv() == data


def test_message_over_limit_closes_1009(srv):
    with websockets.connect(url(srv, "/ws/echo"), max_size=None) as ws:
        ws.send(b"x" * ((1 << 20) + 1))
        with pytest.raises(ConnectionClosed):
            ws.recv()
        assert ws.close_code == 1009


def test_fragments_reassembled_with_ping_between(srv):
    s, rest = upgraded(srv)
    s.sendall(frame(1, b"he", fin=False) + frame(9, b"p!") + frame(0, b"ll", fin=False) + frame(0, b"o"))
    r = Reader(s, rest)
    assert r.frame() == (10, True, b"p!")  # pong first, mid-message
    assert r.frame() == (1, True, b"hello")


def test_bytes_arriving_one_at_a_time(srv):
    s, rest = upgraded(srv)
    data = frame(2, os.urandom(300)) + frame(1, b"ok")
    for i in range(len(data)):
        s.sendall(data[i : i + 1])
    r = Reader(s, rest)
    assert r.frame()[0] == 2
    assert r.frame() == (1, True, b"ok")


@pytest.mark.parametrize(
    "raw,code",
    [
        (frame(1, b"x", mask=False), 1002),  # client frames must be masked
        (frame(1, b"x", rsv=0x40), 1002),  # RSV1 without an extension
        (frame(0, b"x"), 1002),  # continuation with nothing to continue
        (frame(3, b"x"), 1002),  # reserved opcode
        (frame(9, b"x" * 126), 1002),  # control frame over 125 bytes
        (frame(9, b"x", fin=False), 1002),  # fragmented control frame
        (frame(1, b"a", fin=False) + frame(1, b"b"), 1002),  # new message mid-fragment
        (frame(1, b"\xff\xfe"), 1007),  # invalid UTF-8
        (frame(1, b"\xed\xa0\x80"), 1007),  # encoded surrogate
        (frame(8, b"\x03"), 1002),  # one-byte close payload
        (frame(8, struct.pack("!H", 1005)), 1002),  # reserved close code
        (frame(8, struct.pack("!H", 1000) + b"\xff"), 1007),  # bad close reason
    ],
)
def test_protocol_violations_fail_the_connection(srv, raw, code):
    s, rest = upgraded(srv)
    s.sendall(raw)
    assert Reader(s, rest).close_code() == code
    assert eof(s)


def test_close_handshake_echoes_code(srv):
    s, rest = upgraded(srv)
    s.sendall(frame(8, struct.pack("!H", 4001) + b"bye"))
    assert Reader(s, rest).close_code() == 4001
    assert eof(s)
    s, rest = upgraded(srv)
    s.sendall(frame(8))  # no code: answered with 1000
    assert Reader(s, rest).close_code() == 1000


def test_data_after_close_is_ignored(srv):
    s, rest = upgraded(srv)
    s.sendall(frame(8, struct.pack("!H", 1000)) + frame(1, b"late"))
    r = Reader(s, rest)
    assert r.close_code() == 1000
    assert eof(s)


# ---- producer thread (audio-style) ---------------------------------------


def test_stream_from_thread_arrives_in_order(srv):
    frames, size = 3000, 640
    with websockets.connect(url(srv, f"/ws/stream?frames={frames}&size={size}")) as ws:
        for i in range(frames):
            m = ws.recv()
            assert len(m) == size and m[0] == m[-1] == i & 0xFF
        assert json.loads(ws.recv()) == {"sent": frames, "dropped": 0}
        with pytest.raises(ConnectionClosedOK):
            ws.recv()
        assert ws.close_code == 1000 and ws.close_reason == "done"


def test_slow_reader_makes_producer_drop(srv):
    # The producer drops frames while more than 256 KiB are queued. A client
    # that stops reading fills the socket buffers, so drops must happen.
    frames, size = 4000, 65536
    path = f"/ws/stream?frames={frames}&size={size}&max_pending=262144"
    with websockets.connect(url(srv, path), max_size=None, max_queue=1) as ws:
        time.sleep(1.0)
        got = 0
        while True:
            m = ws.recv(timeout=10)
            if isinstance(m, str):
                summary = json.loads(m)
                break
            got += 1
    assert summary["dropped"] > 0
    assert summary["sent"] + summary["dropped"] == frames
    assert got == summary["sent"]


def test_reader_that_stops_is_disconnected(srv):
    # 256 MiB offered, never dropped by the producer. The server queues at
    # most max_pending (16 MiB) and then closes, instead of growing.
    s, rest = upgraded(srv, "/ws/stream?frames=4096&size=65536")
    time.sleep(1.5)
    total = len(rest)
    s.settimeout(10)
    with contextlib.suppress(ConnectionResetError):
        while chunk := s.recv(1 << 20):
            total += len(chunk)
    s.close()
    assert total < 64 << 20
    with websockets.connect(url(srv, "/ws/echo")) as ws:  # server still serves
        ws.send("alive")
        assert ws.recv() == "alive"


def test_client_vanishing_mid_stream(srv):
    s, _ = upgraded(srv, "/ws/stream?frames=100000&size=4096")
    s.recv(65536)
    s.close()
    with websockets.connect(url(srv, "/ws/echo")) as ws:  # server still serves
        ws.send("alive")
        assert ws.recv() == "alive"


def test_many_concurrent_sockets(srv):
    with contextlib.ExitStack() as stack:
        conns = [stack.enter_context(websockets.connect(url(srv, "/ws/echo"))) for _ in range(40)]
        for i, ws in enumerate(conns):
            ws.send(f"m{i}")
        assert [ws.recv() for ws in conns] == [f"m{i}" for i in range(40)]


# ---- full duplex (speech in, audio out, barge-in) ------------------------


def _frame(tag, i, size=640):
    return bytes([tag]) + i.to_bytes(4, "big") + bytes(size - 5)


def test_duplex_round_trip_while_sending(srv):
    n = 500
    sent = [_frame(ord("A"), i) for i in range(n)]
    with websockets.connect(url(srv, "/ws/duplex")) as ws:

        def sender():
            for f in sent:
                ws.send(f)
                time.sleep(0.001)  # roughly real-time: the worker keeps up

        t = threading.Thread(target=sender)
        t.start()
        got = [ws.recv(timeout=10) for _ in range(n)]
        t.join()
        assert got == [f[::-1] for f in sent]  # all back, in order, processed
        ws.send("stats")
        assert json.loads(ws.recv(timeout=5)) == {"dropped": 0}


def test_duplex_slow_worker_drops_oldest(srv):
    n = 200
    with websockets.connect(url(srv, "/ws/duplex?work_us=20000")) as ws:
        for i in range(n):
            ws.send(_frame(ord("A"), i))
        # "stats" is handled after every earlier frame was queued, so its
        # count is final even while the worker is still busy.
        ws.send("stats")
        dropped, frames = None, []
        while dropped is None or len(frames) < n - dropped:
            m = ws.recv(timeout=10)
            if isinstance(m, str):
                dropped = json.loads(m)["dropped"]
            else:
                frames.append(int.from_bytes(m[::-1][1:5], "big"))
        assert dropped > 0
        assert frames == sorted(frames)  # survivors keep their order
        assert frames[-1] == n - 1  # the newest audio survives


def test_duplex_barge_in_discards_stale_output(srv):
    with websockets.connect(url(srv, "/ws/duplex?work_us=5000")) as ws:
        for i in range(60):
            ws.send(_frame(ord("A"), i))
        ws.send("cancel")
        for i in range(20):
            ws.send(_frame(ord("B"), i))
        before, after, cancelled = [], [], None
        while cancelled is None or len(after) < 20:
            m = ws.recv(timeout=10)
            if isinstance(m, str):
                cancelled = json.loads(m)["cancelled"]
            elif cancelled is None:
                before.append(m)
            else:
                after.append(m)
        assert cancelled > 0
        assert len(before) + cancelled == 60  # nothing lost or duplicated
        assert all(m[-1] == ord("B") for m in after)  # no stale speech
