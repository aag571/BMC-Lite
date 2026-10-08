"""Two real daemons exchange generations; peer traffic never writes configured PWM."""
import http.client
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())
tls = len(sys.argv) > 2

def port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]

def wait(check):
    deadline = time.monotonic() + 5
    while not check():
        assert time.monotonic() < deadline, "condition timeout"
        time.sleep(.03)

with tempfile.TemporaryDirectory(prefix="bmc-peer-") as directory:
    root = pathlib.Path(directory)
    token = root / "token"
    token.write_text("x" * 48)
    token.chmod(0o640)
    pwm = root / "pwm"
    pwm.write_text("123\n")
    config = root / "config"
    config.write_text(f"cpu mock 40 1 high 70 90 3 1 3 {pwm}\n")
    cert, key = root / "cert", root / "key"
    if tls:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
    ports = [port(), port()]
    reads = [port(), port()]
    processes = []
    def launch(index, name="localhost", ca=None):
        options = [binary, "--config", str(config), "--rules", str(root / "none"),
                   "--sel", str(root / f"sel{index}"), "--log", str(root / f"log{index}"),
                   "--interval-ms", "10", "--enable-actions", "--http-port", str(reads[index]),
                   "--peer-address", "127.0.0.1", "--peer-port", str(ports[1-index]),
                   "--peer-listen-port", str(ports[index]), "--peer-token-file", str(token),
                   "--peer-interval-ms", "50", "--peer-stale-ms", "500"]
        if tls:
            options += ["--peer-ca", str(ca or cert), "--peer-server-name", name,
                        "--peer-tls-cert", str(cert), "--peer-tls-key", str(key)]
        process = subprocess.Popen(options, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        processes.append(process)
        return process
    def metrics(index):
        try:
            connection = http.client.HTTPConnection("127.0.0.1", reads[index], timeout=1)
            connection.request("GET", "/metrics")
            body = connection.getresponse().read().decode()
            connection.close()
            return body
        except OSError:
            return ""
    def stop(process):
        process.terminate()
        assert process.wait(timeout=3) == 0, process.stderr.read()
    try:
        first, second = launch(0), launch(1)
        wait(lambda: "bmc_peer_generation 1\n" in metrics(0) and "bmc_peer_generation 1\n" in metrics(1))
        second.send_signal(signal.SIGHUP)
        wait(lambda: "bmc_peer_generation 2\n" in metrics(0))
        stop(second)
        wait(lambda: "bmc_peer_stale 1\n" in metrics(0))
        wait(lambda: "peer stale" in (root / "sel0").read_text())
        second = launch(1)
        wait(lambda: "bmc_peer_stale 0\n" in metrics(0))
        assert pwm.read_text() == "123\n"
        # A valid peer token cannot invoke a control action on the peer port.
        if not tls:
            connection = http.client.HTTPConnection("127.0.0.1", ports[0], timeout=1)
            connection.request("POST", "/v1/actions/fan", '{"sensor":"cpu","action":"increase_fan"}',
                               {"Authorization": "Bearer " + "x" * 48})
            assert connection.getresponse().status == 400
            connection.close()
        else:
            # Both outgoing clients use an invalid hostname, so neither direction can refresh liveness.
            stop(first); stop(second)
            first, second = launch(0, "wrong.example"), launch(1, "wrong.example")
            wait(lambda: "bmc_peer_stale 1\n" in metrics(0) and "bmc_peer_stale 1\n" in metrics(1))
            assert "bmc_peer_heartbeats_total 0\n" in metrics(0)
            stop(first); stop(second)
            wrong_ca = root / "wrong-ca"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-subj", "/CN=untrusted", "-keyout", str(root / "wrong-key"), "-out", str(wrong_ca)],
                           check=True, capture_output=True)
            first, second = launch(0, ca=wrong_ca), launch(1, ca=wrong_ca)
            wait(lambda: "bmc_peer_stale 1\n" in metrics(0) and "bmc_peer_stale 1\n" in metrics(1))
            assert "bmc_peer_heartbeats_total 0\n" in metrics(0)
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                assert process.wait(timeout=3) == 0, process.stderr.read()
print("bidirectional heartbeat, reload, stale/recovery, isolated hardware, TLS verification: passed")
