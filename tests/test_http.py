"""HTTP framing, access policy and static files, exercised over real sockets."""

import os
import socket
import stat
import time

import pytest

from conftest import read_response, request


def get(path="/health", host="localhost", extra=""):
    return f"GET {path} HTTP/1.1\r\nHost: {host}\r\n{extra}\r\n".encode()


def closed_by_peer(sock):
    try:
        return sock.recv(1) == b""
    except (ConnectionResetError, socket.timeout):
        return False


# ---- framing -------------------------------------------------------------


def test_keep_alive_serves_sequential_requests(srv):
    with srv.connect() as s:
        for _ in range(3):
            s.sendall(get())
            status, h, body = read_response(s)
            assert status == 200 and body == b'{"status":"ok"}'
            assert "connection" not in h
            assert h["date"].endswith(" GMT")


def test_pipelined_requests_answered_in_order(srv):
    with srv.connect() as s:
        s.sendall(get("/health") + get("/nope") + get("/health"))
        assert [read_response(s)[0] for _ in range(3)] == [200, 404, 200]


def test_connection_close_honoured(srv):
    with srv.connect() as s:
        s.sendall(get(extra="Connection: close\r\n"))
        status, h, _ = read_response(s)
        assert status == 200 and h["connection"] == "close"
        assert closed_by_peer(s)


def test_http10_closes_and_needs_no_host(srv):
    status, h, _ = request(srv, b"GET /health HTTP/1.0\r\n\r\n")
    assert status == 200 and h["connection"] == "close"


def test_unknown_path_and_method(srv):
    assert request(srv, get("/missing"))[0] == 404
    assert request(srv, b"DELETE /health HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 405


def test_head_has_length_but_no_body(srv):
    with srv.connect() as s:
        s.sendall(b"HEAD /health HTTP/1.1\r\nHost: localhost\r\n\r\n" + get())
        status, h, body = read_response(s, head_only=True)
        assert status == 200 and h["content-length"] == "15" and body == b""
        # The next response must start immediately: no body bytes in between.
        assert read_response(s)[2] == b'{"status":"ok"}'


@pytest.mark.parametrize(
    "raw,status",
    [
        (b"GARBAGE\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\n\r\n", 400),  # missing Host
        (b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400),
        (b"GET / HTTP/3.0\r\nHost: localhost\r\n\r\n", 505),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n", 501),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999999\r\n\r\n", 413),
    ],
)
def test_malformed_requests_rejected_and_closed(srv, raw, status):
    with srv.connect() as s:
        s.sendall(raw)
        assert read_response(s)[0] == status
        assert closed_by_peer(s)


def test_oversized_head_gets_431(srv):
    raw = b"GET / HTTP/1.1\r\nHost: localhost\r\nX: " + b"a" * 9000 + b"\r\n\r\n"
    with srv.connect() as s:
        s.sendall(raw)
        assert read_response(s)[0] == 431


def test_413_reaches_client_despite_unread_body(srv):
    # The server closes without reading the body; a lingering close must
    # keep the kernel from resetting the connection before the reply lands.
    with srv.connect() as s:
        s.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 9000000\r\n\r\n")
        s.sendall(b"x" * 200000)
        assert read_response(s)[0] == 413


def test_body_echo_split_across_writes(srv):
    body = os.urandom(300000)
    with srv.connect() as s:
        s.sendall(
            b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/x-test\r\n"
            + f"Content-Length: {len(body)}\r\n\r\n".encode()
        )
        for i in range(0, len(body), 7000):
            s.sendall(body[i : i + 7000])
        status, h, got = read_response(s)
    assert status == 200 and got == body and h["content-type"] == "application/x-test"


def test_expect_100_continue(srv):
    with srv.connect() as s:
        s.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n")
        assert s.recv(64) == b"HTTP/1.1 100 Continue\r\n\r\n"
        s.sendall(b"hello")
        assert read_response(s)[2] == b"hello"


def test_request_deadline_sends_408(spawn):
    srv = spawn("--request-ms", "300")
    with srv.connect() as s:
        s.sendall(b"GET /health HTTP/1.1\r\nHost: loc")
        assert read_response(s)[0] == 408
        assert closed_by_peer(s)


def test_idle_connection_closed(spawn):
    srv = spawn("--idle-ms", "300")
    with srv.connect() as s:
        s.sendall(get())
        read_response(s)
        t = time.time()
        assert closed_by_peer(s)
        assert time.time() - t < 3


def test_many_connections(srv):
    socks = [srv.connect() for _ in range(40)]
    try:
        for s in socks:
            s.sendall(get())
        assert all(read_response(s)[0] == 200 for s in socks)
    finally:
        for s in socks:
            s.close()


# ---- access policy -------------------------------------------------------


@pytest.mark.parametrize("host", ["localhost", "localhost:1234", "127.0.0.1", "[::1]:80", "ui.localhost"])
def test_loopback_hosts_accepted(srv, host):
    assert request(srv, get(host=host))[0] == 200


@pytest.mark.parametrize("host", ["evil.com", "localhost.evil.com", "10.0.0.1", "127.0.0.1.nip.io"])
def test_foreign_hosts_rejected(srv, host):
    # DNS rebinding: the browser sends the attacker's name as Host.
    assert request(srv, get(host=host))[0] == 403


def test_cross_origin_rejected(srv):
    assert request(srv, get(extra="Origin: https://evil.com\r\n"))[0] == 403
    assert request(srv, get(extra="Origin: null\r\n"))[0] == 403


def test_local_origin_gets_cors_headers(srv):
    status, h, _ = request(srv, get(extra="Origin: http://localhost:5173\r\n"))
    assert status == 200
    assert h["access-control-allow-origin"] == "http://localhost:5173"
    assert h["vary"] == "Origin"


def test_extra_origin_allowed(spawn):
    srv = spawn("--origin", "https://app.example")
    status, h, _ = request(srv, get(extra="Origin: https://app.example\r\n"))
    assert status == 200 and h["access-control-allow-origin"] == "https://app.example"
    assert request(srv, get(extra="Origin: https://app.example.org\r\n"))[0] == 403


def test_preflight(spawn):
    srv = spawn("--token", "s3cret")
    raw = (
        b"OPTIONS /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\n"
        b"Origin: http://localhost:3000\r\nAccess-Control-Request-Method: POST\r\n"
        b"Access-Control-Request-Headers: authorization, content-type\r\n\r\n"
    )
    status, h, _ = request(srv, raw)
    assert status == 204  # preflights carry no credentials
    assert h["access-control-allow-methods"] == "POST"
    assert h["access-control-allow-headers"] == "authorization, content-type"
    assert h["access-control-allow-origin"] == "http://localhost:3000"


def test_bearer_token(spawn):
    srv = spawn("--token", "s3cret")
    status, h, _ = request(srv, get())
    assert status == 401 and h["www-authenticate"] == "Bearer"
    assert request(srv, get(extra="Authorization: Bearer wrong\r\n"))[0] == 401
    assert request(srv, get(extra="Authorization: Bearer s3cret2\r\n"))[0] == 401
    assert request(srv, get(extra="Authorization: bearer s3cret\r\n"))[0] == 200


def test_token_rejection_keeps_pipelined_request(spawn):
    srv = spawn("--token", "t")
    with srv.connect() as s:
        s.sendall(get() + get(extra="Authorization: Bearer t\r\n"))
        assert read_response(s)[0] == 401
        assert read_response(s)[0] == 200


def test_unix_socket(spawn, tmp_path):
    path = str(tmp_path / "spore.sock")
    srv = spawn("--unix", path)
    assert stat.S_IMODE(os.stat(path).st_mode) == 0o600
    # Browsers cannot reach a Unix socket, so Host is not restricted.
    assert request(srv, get(host="anything.example"))[0] == 200
    assert srv.stop() == 0
    assert not os.path.exists(path)


def test_ipv6_loopback(spawn):
    if not socket.has_ipv6:
        pytest.skip("no IPv6")
    try:
        srv = spawn("--ipv6")
    except RuntimeError:
        pytest.skip("::1 unavailable")
    with socket.create_connection(("::1", srv.port), timeout=5) as s:
        s.sendall(get(host="[::1]"))
        assert read_response(s)[0] == 200


def test_not_reachable_on_other_interfaces(srv):
    # UDP connect() picks the outbound interface without sending anything.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
        try:
            u.connect(("192.0.2.1", 9))  # TEST-NET-1
        except OSError:
            pytest.skip("no route off-host")
        addr = u.getsockname()[0]
    if addr.startswith("127."):
        pytest.skip("no non-loopback IPv4 address")
    with pytest.raises(OSError):
        socket.create_connection((addr, srv.port), timeout=1).close()


# ---- static files --------------------------------------------------------


@pytest.fixture
def site(tmp_path, spawn):
    (tmp_path / "index.html").write_text("<h1>home</h1>")
    (tmp_path / "app.js").write_text("console.log(1)")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "index.html").write_text("sub")
    (tmp_path / "a b.txt").write_text("spaced")
    return spawn("--static", str(tmp_path))


def test_static_files(site):
    status, h, body = request(site, get("/"))
    assert status == 200 and body == b"<h1>home</h1>"
    assert h["content-type"].startswith("text/html")
    status, h, body = request(site, get("/app.js"))
    assert body == b"console.log(1)" and h["content-type"].startswith("text/javascript")
    assert request(site, get("/a%20b.txt"))[2] == b"spaced"
    assert request(site, get("/sub/"))[2] == b"sub"


def test_static_directory_redirect(site):
    status, h, _ = request(site, get("/sub"))
    assert status == 301 and h["location"] == "/sub/"


@pytest.mark.parametrize("path", ["/../etc/passwd", "/%2e%2e/etc/passwd", "/sub/../../x", "/%00"])
def test_static_traversal_rejected(site, path):
    assert request(site, get(path))[0] == 400


def test_static_missing_and_api_routes_still_win(site):
    assert request(site, get("/nope.txt"))[0] == 404
    assert request(site, get("/health"))[2] == b'{"status":"ok"}'
