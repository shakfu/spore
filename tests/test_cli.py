"""The spore command line: subcommands, argument checks, llama-server names."""

import json
import os
import re
import struct
import subprocess
from pathlib import Path

import pytest

from conftest import MODULES, SPORE, request, requires


def run(*args, exe=SPORE):
    return subprocess.run([exe, *args], capture_output=True, text=True, timeout=10)


def get(path="/health", extra=""):
    return f"GET {path} HTTP/1.1\r\nHost: localhost\r\n{extra}\r\n".encode()


def test_version_matches_header():
    header = (Path(__file__).parents[1] / "include" / "spore.h").read_text()
    version = re.search(r'#define SPORE_VERSION "([^"]+)"', header).group(1)
    for args in (["version"], ["--version"]):
        p = run(*args)
        assert p.returncode == 0 and p.stdout == f"spore {version}\n"


def test_help_goes_to_stdout_and_usage_errors_to_stderr():
    for args in (["--help"], ["help"], ["serve", "--help"], ["token", "--help"]):
        p = run(*args)
        assert p.returncode == 0 and "usage: spore" in p.stdout and not p.stderr, args
    for args in ([], ["bogus"], ["token"], ["version", "x"]):
        p = run(*args)
        assert p.returncode == 2 and "usage: spore" in p.stderr and not p.stdout, args
    assert "unknown command 'bogus'" in run("bogus").stderr


def test_modules_lists_core_first():
    assert run("modules").stdout.split()[0] == "core"


@pytest.mark.parametrize("args,needle", [
    (["--port", "70000"], "--port must be an integer in 0..65535, not '70000'"),
    (["--port", "80x"], "--port"),
    (["--idle-ms", "5s"], "--idle-ms"),
    (["--idle-ms", "0"], "--idle-ms"),
    (["--grace-ms", "-1"], "--grace-ms"),
    (["--port"], "--port needs a value"),
    (["--jinja"], "unknown option --jinja"),
    (["--static", "/tmp"], "unknown option --static"),  # renamed --path
    (["--api-key", "a,b"], "one key"),
    (["--api-key", "a", "--api-key-file", "/dev/null"], "exclusive"),
    (sum((["--origin", f"http://a{i}"] for i in range(16)), []), "--origin"),
])
def test_serve_rejects_bad_arguments(args, needle):
    p = run("serve", *args)
    assert p.returncode == 2 and needle in p.stderr, p.stderr


@requires("llm")
@pytest.mark.parametrize("args", [["--workers", "-1"], ["--workers", "100000"], ["--queue", "0"]])
def test_serve_rejects_bad_worker_counts(args):
    p = run("serve", *args)
    assert p.returncode == 2 and args[0] in p.stderr


@pytest.mark.parametrize("host", ["0.0.0.0", "192.168.1.5", "::", "example.com", "127.0.0.1,::1"])
def test_host_refuses_non_loopback(host):
    p = run("serve", "--host", host)
    assert p.returncode == 2 and "docs/dev/net-security.md" in p.stderr


@pytest.mark.parametrize("host,addr", [("127.0.0.1", "http://127.0.0.1:"),
                                       ("localhost", "http://127.0.0.1:"),
                                       ("::1", "http://[::1]:")])
def test_host_binds_loopback(host, addr):
    p = subprocess.Popen([SPORE, "serve", "--port", "0", "--host", host],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        line = p.stdout.readline()
        if host == "::1" and not line:
            pytest.skip(f"no IPv6 loopback: {p.stderr.read()}")
        assert line.startswith(f"spore listening on {addr}"), line
    finally:
        p.terminate()
        assert p.wait(timeout=5) == 0


def test_host_ending_in_sock_is_a_unix_socket(spawn, tmp_path):
    path = tmp_path / "s.sock"
    srv = spawn("--host", str(path))
    assert srv.unix == str(path)
    assert request(srv, get())[0] == 200


@pytest.mark.parametrize("content", ["", "a b\n", "k1\nk2\n", "a,b\n"])
def test_api_key_file_holds_one_key(tmp_path, content):
    path = tmp_path / "key"
    path.write_text(content)
    p = run("serve", "--api-key-file", str(path))
    assert p.returncode == 2 and "one key on one line" in p.stderr


def test_api_key_file_missing():
    p = run("serve", "--api-key-file", "/nonexistent/key")
    assert p.returncode == 1 and "No such file" in p.stderr


def test_api_key_file_written_by_hand(spawn, tmp_path):
    path = tmp_path / "key"
    path.write_text("s3cret\n")
    srv = spawn("--api-key-file", str(path))
    assert request(srv, get())[0] == 401
    assert request(srv, get(extra="Authorization: Bearer s3cret\r\n"))[0] == 200


def test_token_refuses_unwritable_path():
    p = run("token", "/nonexistent/dir/key")
    assert p.returncode == 1 and "cannot write" in p.stderr


def test_llama_server_link_runs_serve(tmp_path):
    link = tmp_path / "llama-server"
    link.symlink_to(SPORE)
    p = subprocess.Popen([str(link), "--port", "0", "--api-key", "k"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert p.stdout.readline().startswith("spore listening on http://127.0.0.1:")
    finally:
        p.terminate()
        assert p.wait(timeout=5) == 0
    p = run("--jinja", exe=str(link))
    assert p.returncode == 2 and "unknown option --jinja" in p.stderr


@requires("llm")
def test_alias_names_the_model(spawn):
    srv = spawn("--alias", "my-model")
    status, _, body = request(srv, get("/v1/models"))
    assert status == 200 and [m["id"] for m in json.loads(body)["data"]] == ["my-model"]


@pytest.mark.skipif("whisper" in MODULES, reason="built with whisper.cpp")
def test_transcribe_says_how_to_build_it():
    p = run("transcribe", "-m", "x", "a.wav")
    assert p.returncode == 2 and "make engines" in p.stderr


@pytest.mark.skipif("outetts" in MODULES, reason="built with OuteTTS")
def test_speak_says_how_to_build_it():
    p = run("speak", "-m", "x", "--vocoder", "y", "hi")
    assert p.returncode == 2 and "make engines" in p.stderr


def wav(path, bits=16, fmt=1, channels=1, rate=16000, data=b"\0\0" * 160):
    fmt_chunk = struct.pack("<HHIIHH", fmt, channels, rate, rate * channels * bits // 8,
                            channels * bits // 8, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt_chunk + b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
    return str(path)


@requires("whisper")
@pytest.mark.parametrize("make,needle", [
    (lambda p: (p.write_text("not audio"), str(p))[1], "not a RIFF WAVE file"),
    (lambda p: wav(p, bits=8, data=b"\x80" * 160), "only 16-bit PCM WAV"),
    (lambda p: wav(p, fmt=3, bits=32, data=b"\0" * 640), "only 16-bit PCM WAV"),
    (lambda p: "/nonexistent.wav", "No such file"),
])
def test_transcribe_rejects_bad_input_before_loading(tmp_path, make, needle):
    p = run("transcribe", "-m", "/nonexistent/model.bin", make(tmp_path / "a.wav"))
    assert p.returncode == 1 and needle in p.stderr, p.stderr


@requires("whisper")
def test_transcribe_needs_model_and_file():
    assert run("transcribe", "a.wav").returncode == 2
    assert run("transcribe", "-m", "m").returncode == 2


@requires("outetts")
def test_speak_needs_model_vocoder_and_text():
    assert run("speak", "-m", "m", "hi").returncode == 2
    assert run("speak", "-m", "m", "--vocoder", "v").returncode == 2
