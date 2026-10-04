"""HTTP framing, access policy and static files, exercised over real sockets."""

import os
import re
import signal
import socket
import stat
import time

import pytest

from conftest import read_response, request, wait_for


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
    status, h, _ = request(srv, b"DELETE /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
    assert status == 405 and h["allow"] == "GET, HEAD"
    status, h, _ = request(srv, get("/echo"))
    assert status == 405 and h["allow"] == "POST"


def test_accept_backs_off_when_out_of_descriptors(spawn):
    # 16 descriptors fit about 10 connections. Before the backoff, the
    # listener stayed readable while accept() failed with EMFILE: one core spun.
    srv = spawn(nofile=16)
    conns = [srv.connect() for _ in range(30)]
    try:
        time.sleep(0.2)
        stat = f"/proc/{srv.proc.pid}/stat"
        if not os.path.exists(stat):
            pytest.skip("needs /proc")

        def cpu():
            f = open(stat).read().rsplit(")", 1)[1].split()
            return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")

        t0 = cpu()
        time.sleep(1.0)
        assert cpu() - t0 < 0.2
    finally:
        for c in conns:
            c.close()
    assert wait_for(lambda: request(srv, get())[0] == 200)  # recovers


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
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", 400),
        (b"POST /echo HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5\nhello\r\n0\r\n\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5;a\nb\r\nhello\r\n0\r\n\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFFFF\r\n", 413),
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


def test_body_that_grows_buffer_with_six_letter_header(srv):
    # Regression: a 6-letter header name made the Expect lookup read the
    # head from the buffer the body realloc had freed (caught under ASan).
    body = b"y" * 100000
    with srv.connect() as s:
        s.sendall(
            b"POST /echo HTTP/1.1\r\nHost: localhost\r\nAccept: */*\r\n"
            + f"Content-Length: {len(body)}\r\n\r\n".encode()
            + body
        )
        assert read_response(s)[2] == body


def test_expect_100_continue(srv):
    with srv.connect() as s:
        s.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n")
        assert s.recv(64) == b"HTTP/1.1 100 Continue\r\n\r\n"
        s.sendall(b"hello")
        assert read_response(s)[2] == b"hello"


def chunked(body, size=7000):
    out = b""
    for i in range(0, len(body), size):
        part = body[i : i + size]
        out += f"{len(part):x};n={i}\r\n".encode() + part + b"\r\n"
    return out + b"0\r\nX-Trailer: t\r\n\r\n"


CHUNKED_POST = b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"


def test_chunked_body_split_across_writes(srv):
    body = os.urandom(300000)
    raw = chunked(body)
    with srv.connect() as s:
        s.sendall(CHUNKED_POST + b"\r\n")
        for i in range(0, len(raw), 4093):
            s.sendall(raw[i : i + 4093])
        s.sendall(get())  # pipelined after the body
        status, h, got = read_response(s)
        assert status == 200 and got == body
        assert read_response(s)[0] == 200


def test_chunked_expect_100_continue(srv):
    with srv.connect() as s:
        s.sendall(CHUNKED_POST + b"Expect: 100-continue\r\n\r\n")
        assert s.recv(64) == b"HTTP/1.1 100 Continue\r\n\r\n"
        s.sendall(b"5\r\nhello\r\n0\r\n\r\n")
        assert read_response(s)[2] == b"hello"


def test_chunked_body_over_limit_gets_413(srv):
    # The second size line takes the body past the 8 MiB default.
    with srv.connect() as s:
        s.sendall(CHUNKED_POST + b"\r\n500000\r\n" + b"x" * 0x500000 + b"\r\n500000\r\n")
        assert read_response(s)[0] == 413
        assert closed_by_peer(s)


def test_chunked_body_not_read_as_request_after_401(spawn):
    srv = spawn("--token", "t")
    inner = get()
    with srv.connect() as s:
        s.sendall(CHUNKED_POST + b"\r\n" + f"{len(inner):x}\r\n".encode() + inner + b"\r\n0\r\n\r\n")
        status, h, _ = read_response(s)
        assert status == 401 and h["connection"] == "close"
        assert closed_by_peer(s)  # no second response


def test_request_deadline_sends_408(spawn):
    srv = spawn("--request-ms", "300")
    with srv.connect() as s:
        s.sendall(b"GET /health HTTP/1.1\r\nHost: loc")
        assert read_response(s)[0] == 408
        assert closed_by_peer(s)


def test_idle_connection_closed(spawn):
    srv = spawn("--idle-ms", "200")
    with srv.connect() as s:
        s.sendall(get())
        read_response(s)
        t = time.time()
        assert closed_by_peer(s)
        # The poll wait ends at the deadline, not at a 1 s tick.
        assert 0.15 < time.time() - t < 0.6


def test_half_close_still_answered(srv):
    with srv.connect() as s:
        s.sendall(get() + get("/nope"))
        s.shutdown(socket.SHUT_WR)
        assert read_response(s)[0] == 200
        assert read_response(s)[0] == 404
        assert closed_by_peer(s)


def test_half_close_with_partial_request_closes(srv):
    with srv.connect() as s:
        s.sendall(b"GET /health HTTP/1.1\r\nHost: loc")
        s.shutdown(socket.SHUT_WR)
        t = time.time()
        assert closed_by_peer(s)
        assert time.time() - t < 0.5


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


@pytest.mark.parametrize(
    "fetch,status",
    [
        ("Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: no-cors\r\nSec-Fetch-Dest: image\r\n", 403),
        ("Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Dest: object\r\n", 403),
        ("Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Dest: document\r\n", 200),
        ("Sec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\nSec-Fetch-Dest: iframe\r\n", 200),
        ("Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: no-cors\r\n", 200),
        ("Sec-Fetch-Site: none\r\nSec-Fetch-Mode: navigate\r\n", 200),
    ],
)
def test_cross_site_loads_without_origin(srv, fetch, status):
    assert request(srv, get(extra=fetch))[0] == status


def test_cross_site_navigation_post_rejected(srv):
    raw = b"POST /echo HTTP/1.1\r\nHost: localhost\r\nSec-Fetch-Site: cross-site\r\nSec-Fetch-Mode: navigate\r\nContent-Length: 0\r\n\r\n"
    assert request(srv, raw)[0] == 403


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


def test_origins_only(spawn):
    srv = spawn("--origins-only", "--origin", "https://app.example")
    assert request(srv, get(extra="Origin: https://app.example\r\n"))[0] == 200
    assert request(srv, get(extra="Origin: http://localhost:5173\r\n"))[0] == 403
    assert request(srv, get())[0] == 200  # no Origin: not a browser request


def test_extra_hosts(spawn):
    srv = spawn("--host", "spore.example.com")
    assert request(srv, get(host="Spore.Example.com"))[0] == 200
    assert request(srv, get(host="spore.example.com:443"))[0] == 200
    assert request(srv, get(host="localhost"))[0] == 200
    assert request(srv, get(host="evil.com"))[0] == 403


def test_extra_hosts_checked_on_unix_socket(spawn, tmp_path):
    srv = spawn("--unix", str(tmp_path / "s.sock"), "--host", "spore.example.com")
    assert request(srv, get(host="spore.example.com"))[0] == 200
    assert request(srv, get(host="anything.example"))[0] == 403


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


def test_token_file(spawn, tmp_path):
    path = tmp_path / "token"
    path.write_text("old")
    os.chmod(path, 0o644)
    srv = spawn("--token-file", str(path))
    token = path.read_text().strip()
    assert stat.S_IMODE(os.stat(path).st_mode) == 0o600
    assert len(token) == 64 and int(token, 16) >= 0
    assert srv.proc.stdout.readline().strip() == f"open http://localhost:{srv.port}/#token={token}"
    assert request(srv, get())[0] == 401
    assert request(srv, get(extra=f"Authorization: Bearer {token}\r\n"))[0] == 200
    assert [p.name for p in tmp_path.iterdir()] == ["token"]  # no temp file left


def test_unix_socket_in_use_is_not_taken_over(spawn, tmp_path):
    path = str(tmp_path / "spore.sock")
    first = spawn("--unix", path)
    with pytest.raises(RuntimeError):
        spawn("--unix", path)
    assert request(first, get(host="x"))[0] == 200


def test_stale_unix_socket_replaced(spawn, tmp_path):
    path = str(tmp_path / "spore.sock")
    old = spawn("--unix", path)
    old.proc.kill()  # no cleanup: the socket file stays
    old.proc.wait()
    assert os.path.exists(path)
    assert request(spawn("--unix", path), get(host="x"))[0] == 200


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
    (tmp_path / ".env").write_text("SECRET=1")
    (tmp_path / ".git").mkdir()
    (tmp_path / ".git" / "config").write_text("[core]")
    (tmp_path / "sub" / ".hidden").write_text("h")
    return spawn("--static", str(tmp_path))


def test_static_files(site):
    status, h, body = request(site, get("/"))
    assert status == 200 and body == b"<h1>home</h1>"
    assert h["content-type"].startswith("text/html")
    status, h, body = request(site, get("/app.js"))
    assert body == b"console.log(1)" and h["content-type"].startswith("text/javascript")
    assert request(site, get("/a%20b.txt"))[2] == b"spaced"
    assert request(site, get("/sub/"))[2] == b"sub"
    assert h["x-content-type-options"] == "nosniff"


@pytest.mark.parametrize("path", ["/.env", "/.git/config", "/.git/", "/sub/.hidden", "/%2eenv", "/./app.js"])
def test_static_hidden_files_not_served(site, path):
    assert request(site, get(path))[0] == 404


def test_static_directory_redirect(site):
    status, h, _ = request(site, get("/sub"))
    assert status == 301 and h["location"] == "/sub/"
    # "//sub/" would be a scheme-relative URL to the host "sub".
    status, h, _ = request(site, get("//sub"))
    assert status == 301 and h["location"] == "/sub/"


@pytest.mark.parametrize("path", ["/../etc/passwd", "/%2e%2e/etc/passwd", "/sub/../../x", "/%00"])
def test_static_traversal_rejected(site, path):
    assert request(site, get(path))[0] == 400


def test_static_missing_and_api_routes_still_win(site):
    assert request(site, get("/nope.txt"))[0] == 404
    assert request(site, get("/health"))[2] == b'{"status":"ok"}'


@pytest.fixture
def media(tmp_path, spawn):
    data = bytes(range(256)) * 4
    (tmp_path / "a.bin").write_bytes(data)
    (tmp_path / "empty.bin").write_bytes(b"")
    return spawn("--static", str(tmp_path)), data


@pytest.mark.parametrize(
    "rng,first,last",
    [
        ("bytes=0-3", 0, 3),
        ("bytes=1000-", 1000, 1023),
        ("bytes=-24", 1000, 1023),
        ("bytes=1020-5000", 1020, 1023),
        ("bytes=-5000", 0, 1023),
        ("BYTES=2-2", 2, 2),
    ],
)
def test_static_range(media, rng, first, last):
    srv, data = media
    status, h, body = request(srv, get("/a.bin", extra=f"Range: {rng}\r\n"))
    assert status == 206 and body == data[first : last + 1]
    assert h["content-range"] == f"bytes {first}-{last}/1024"
    assert h["accept-ranges"] == "bytes"


@pytest.mark.parametrize("path,rng,size", [("/a.bin", "bytes=1024-", 1024), ("/a.bin", "bytes=-0", 1024), ("/empty.bin", "bytes=0-", 0)])
def test_static_range_unsatisfiable(media, path, rng, size):
    status, h, _ = request(media[0], get(path, extra=f"Range: {rng}\r\n"))
    assert status == 416 and h["content-range"] == f"bytes */{size}"


@pytest.mark.parametrize(
    "extra",
    [
        "Range: bytes=0-1,5-6\r\n",  # several ranges: whole file is allowed
        "Range: bytes=3-1\r\n",
        "Range: items=0-1\r\n",
        "Range: bytes=x-1\r\n",
        'Range: bytes=0-1\r\nIf-Range: "v1"\r\n',  # no validators to match
    ],
)
def test_static_range_ignored(media, extra):
    srv, data = media
    status, h, body = request(srv, get("/a.bin", extra=extra))
    assert status == 200 and body == data and h["accept-ranges"] == "bytes"


def test_static_head_range(media):
    srv, _ = media
    with srv.connect() as s:
        s.sendall(b"HEAD /a.bin HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-3\r\n\r\n" + get())
        status, h, _ = read_response(s, head_only=True)
        assert status == 206 and h["content-length"] == "4"
        assert read_response(s)[0] == 200  # no body bytes in between


# ---- access log ------------------------------------------------------------


def test_access_log(spawn):
    srv = spawn("--log")
    request(srv, get("/health"))
    request(srv, get("/missing?q=1"))
    request(srv, get("/health", host="evil.com"))
    request(srv, b"GARBAGE\r\n\r\n")
    request(srv, CHUNKED_POST + b"\r\n3\r\nabc\r\n0\r\n\r\n")
    assert srv.stop() == 0
    lines = srv.proc.stderr.read().splitlines()
    pats = [
        r"spored: GET /health 200 (\d+) \d+ms$",
        r"spored: GET /missing\?q=1 404 \d+ \d+ms route$",
        r"spored: GET /health 403 \d+ \d+ms host$",
        r"spored:   400 \d+ \d+ms parse$",
        r"spored: POST /echo 200 \d+ \d+ms$",
    ]
    assert len(lines) == len(pats), lines
    for line, pat in zip(lines, pats):
        assert re.match(pat, line), (line, pat)
    assert int(re.match(pats[0], lines[0]).group(1)) > 15  # head + body


# ---- graceful shutdown -------------------------------------------------------


def test_shutdown_closes_idle_connections(srv):
    with srv.connect() as s:
        s.sendall(get())
        read_response(s)
        t = time.time()
        srv.proc.send_signal(signal.SIGTERM)
        assert closed_by_peer(s)
        assert srv.proc.wait(timeout=5) == 0
        assert time.time() - t < 1


def test_shutdown_finishes_request_in_progress(srv):
    with srv.connect() as s:
        s.sendall(b"GET /health HTTP/1.1\r\nHost: loc")
        time.sleep(0.1)
        srv.proc.send_signal(signal.SIGTERM)
        time.sleep(0.2)
        with pytest.raises(ConnectionRefusedError):
            srv.connect()  # the listener is closed first
        s.sendall(b"alhost\r\n\r\n")
        status, h, _ = read_response(s)
        assert status == 200 and h["connection"] == "close"
        assert closed_by_peer(s)
    assert srv.proc.wait(timeout=5) == 0


def test_shutdown_deadline(spawn):
    srv = spawn("--grace-ms", "300")
    with srv.connect() as s:
        s.sendall(b"GET /health HTTP/1.1\r\nHost: loc")
        time.sleep(0.1)
        t = time.time()
        srv.proc.send_signal(signal.SIGTERM)
        assert srv.proc.wait(timeout=5) == 0
        assert 0.25 < time.time() - t < 1.5


def test_second_signal_ends_shutdown(srv):
    with srv.connect() as s:
        s.sendall(b"GET /health HTTP/1.1\r\nHost: loc")
        time.sleep(0.1)
        srv.proc.send_signal(signal.SIGTERM)
        time.sleep(0.2)
        t = time.time()
        srv.proc.send_signal(signal.SIGTERM)
        assert srv.proc.wait(timeout=5) == 0
        assert time.time() - t < 1
