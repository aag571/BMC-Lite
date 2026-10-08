"""Control HTTP/HTTPS integration. Hardware writes target a temporary regular file."""
import http.client
import json
import os
import pathlib
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())
tls_enabled = len(sys.argv) > 2 and sys.argv[2] == "tls"

def unused_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]

def wait_for(check):
    deadline = time.monotonic() + 4
    while not check():
        assert time.monotonic() < deadline, "condition timeout"
        time.sleep(.02)

with tempfile.TemporaryDirectory(prefix="bmc-control-") as directory:
    root = pathlib.Path(directory)
    config, token_file, pwm = root / "config", root / "token", root / "pwm"
    token = "x" * 48
    token_file.write_text(token + "\n")
    token_file.chmod(0o640)
    pwm.write_text("0\n")
    config.write_text(f"cpu mock 40 1 high 70 90 3 1 3 {pwm}\n")
    sel = root / "sel"
    common = [binary, "--config", str(config), "--rules", str(root / "none"),
              "--sel", str(sel), "--log", str(root / "log"), "--interval-ms", "10",
              "--enable-actions"]
    def rejected(options):
        result = subprocess.run(common + ["--ticks", "3"] + options, capture_output=True, timeout=3)
        assert result.returncode == 1 and b"control unavailable" in result.stderr, result
    port = unused_port()
    rejected(["--control-port", str(port)])
    rejected(["--control-port", str(port), "--control-token-file", str(root / "missing-token")])
    token_file.chmod(0o644)
    rejected(["--control-port", str(port), "--control-token-file", str(token_file)])
    token_file.chmod(0o640)
    rejected(["--control-port", str(port), "--control-token-file", str(token_file), "--control-bind", "0.0.0.0"])
    certificate, key = root / "cert.pem", root / "key.pem"
    context = None
    options = []
    if tls_enabled:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(certificate), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, capture_output=True)
        context = ssl.create_default_context(cafile=str(certificate))
        options = ["--control-tls-cert", str(certificate), "--control-tls-key", str(key),
                   "--control-bind", "0.0.0.0"]
    process = subprocess.Popen(common + ["--control-port", str(port), "--control-token-file", str(token_file)] + options,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    def request(method, target, body=None, authorization=token):
        if context:
            connection = http.client.HTTPSConnection("localhost", port, context=context, timeout=2)
        else:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
        headers = {"Content-Type": "application/json"}
        if authorization is not None:
            headers["Authorization"] = "Bearer " + authorization
        connection.request(method, target, body=body, headers=headers)
        result = connection.getresponse()
        data = result.read()
        connection.close()
        return result.status, data
    try:
        def started():
            assert process.poll() is None
            try:
                return request("GET", "/v1/config/generation")[0] == 200
            except OSError:
                return False
        wait_for(started)
        assert json.loads(request("GET", "/v1/config/generation")[1])["generation"] == 1
        for index in range(5):
            assert request("GET", "/v1/config/generation", authorization=None)[0] == 401
        assert request("GET", "/v1/config/generation", authorization="wrong")[0] == 429
        assert request("POST", "/v1/actions/fan", '{"sensor":"cpu","path":"/tmp/evil"}')[0] == 400
        payload = '{"sensor":"cpu","action":"increase_fan"}'
        assert request("POST", "/v1/actions/fan", payload)[0] == 202
        wait_for(lambda: pwm.read_text() == "255\n")
        pwm.write_text("123\n")
        assert request("POST", "/v1/actions/fan", payload)[0] == 202
        time.sleep(.1)
        assert pwm.read_text() == "123\n", "cooldown was bypassed"
        assert request("POST", "/v1/actions/inspect", '{"sensor":"cpu","action":"inspect_device"}')[0] == 202
        config.write_text(f"other mock 40 1 high 70 90 3 1 3 {pwm}\n")
        process.send_signal(signal.SIGHUP)
        wait_for(lambda: json.loads(request("GET", "/v1/config/generation")[1])["generation"] == 2)
        assert request("POST", "/v1/actions/fan", payload)[0] == 400
        if context:
            # 证书链和主机名必须由客户端验证；错误信任根及错误主机名都会失败。
            for host, verify_context in [("localhost", ssl.create_default_context()), ("127.0.0.1", context)]:
                connection = http.client.HTTPSConnection(host, port, context=verify_context, timeout=2)
                try:
                    connection.request("GET", "/v1/config/generation")
                    raise AssertionError("invalid certificate was accepted")
                except ssl.SSLCertVerificationError:
                    pass
                finally:
                    connection.close()
        history = sel.read_text()
        raw = socket.create_connection(("127.0.0.1", port), timeout=2)
        if context:
            raw = context.wrap_socket(raw, server_hostname="localhost")
        raw.sendall(b"broken\r\n\r\n")
        assert b"400 Bad Request" in raw.recv(4096)
        raw.close()
        clients = []
        try:
            for index in range(8):
                clients.append(socket.create_connection(("127.0.0.1", port), timeout=2))
                time.sleep(.02)
            extra = socket.create_connection(("127.0.0.1", port), timeout=2)
            try:
                assert extra.recv(1) == b""
            finally:
                extra.close()
        finally:
            for client in clients:
                client.close()
        wait_for(lambda: "connection-limit" in sel.read_text() and "detail=\\\"parse\\\"" in sel.read_text())
        time.sleep(.1)
        slow = socket.create_connection(("127.0.0.1", port), timeout=6)
        if not context:
            slow.sendall(b"GET / HTTP/1.1\r\nHost:")
        try:
            assert slow.recv(1) == b""
        finally:
            slow.close()
        wait_for(lambda: "timeout" in sel.read_text())
        history = sel.read_text()
        for outcome in ["accepted", "rejected", "unauthorized", "rate-limited"]:
            assert f"outcome={outcome}" in history, history
        assert "peer=" in history and "request_id=" in history
        assert token not in history
    finally:
        process.terminate()
        _, errors = process.communicate(timeout=4)
        assert process.returncode == 0, errors
print("control credentials, audit, Worker/PWM cooldown, reload and certificate verification: passed")
