"""Real TCP collector stall/reconnect must not stall daemon sampling."""
import http.client
import json
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())
def port_number():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]
with tempfile.TemporaryDirectory(prefix="bmc-uplink-") as directory:
    root = pathlib.Path(directory)
    config = root / "config"
    config.write_text("cpu mock 40,95 1 high 70 90 3 1 1 -\n")
    rules = root / "rules"
    rules.write_text("inspect cpu critical 1 1 inspect_device\n")
    http_port, collector_port = port_number(), port_number()
    while collector_port == http_port:
        collector_port = port_number()
    # Start daemon while collector is absent to exercise failed connects/backoff.
    process = subprocess.Popen([binary, "--config", str(config), "--rules", str(rules),
        "--log", str(root / "log"), "--sel", str(root / "sel"),
        "--interval-ms", "1", "--http-port", str(http_port),
        "--uplink-address", "127.0.0.1", "--uplink-port", str(collector_port),
        "--uplink-capacity", "4"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    listener, collector = None, None
    def get(target):
        connection = http.client.HTTPConnection("127.0.0.1", http_port, timeout=2)
        connection.request("GET", target)
        response = connection.getresponse()
        data = response.read()
        assert response.status == 200
        connection.close()
        return data
    def metrics():
        return {line.split()[0]: int(line.split()[1]) for line in get("/metrics").decode().splitlines()
                if line.startswith("bmc_uplink_")}
    deadline = time.monotonic() + 4
    try:
        while True:
            try:
                state = metrics()
                break
            except OSError:
                assert process.poll() is None and time.monotonic() < deadline
                time.sleep(.02)
        initial = json.loads(get("/healthz"))["samples"]
        time.sleep(.2)
        assert json.loads(get("/healthz"))["samples"] > initial
        assert metrics()["bmc_uplink_queued"] <= 4
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        listener.bind(("127.0.0.1", collector_port))
        listener.listen()
        listener.settimeout(4)
        collector, _ = listener.accept()
        # Collector deliberately stops reading. Queue stays bounded while ticks progress.
        previous = json.loads(get("/healthz"))["samples"]
        for index in range(6):
            time.sleep(.2)
            state = metrics()
            assert state["bmc_uplink_queued"] <= 4
            current = json.loads(get("/healthz"))["samples"]
            assert current > previous
            previous = current
        assert state["bmc_uplink_dropped_total"] > 0
        attempts = state["bmc_uplink_connect_attempts_total"]
        collector.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        collector.close()
        collector, _ = listener.accept()
        collector.settimeout(4)
        process.send_signal(signal.SIGHUP)
        data = b""
        records = []
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline and not records:
            data += collector.recv(8192)
            while b"\n" in data:
                line, data = data.split(b"\n", 1)
                records.append(json.loads(line))
        assert records
        for record in records:
            assert set(record) == {"type", "source", "message", "sequence", "time_ms", "value"}
            assert record["type"] in {"sensor_state", "configuration", "service", "recovery"}
        assert metrics()["bmc_uplink_connect_attempts_total"] > attempts
    finally:
        if collector:
            collector.close()
        if listener:
            listener.close()
        process.terminate()
        _, errors = process.communicate(timeout=4)
        assert process.returncode == 0, errors
print("absent/stalled collector, bounded queue, sampling progress, reconnect and JSONL: passed")
