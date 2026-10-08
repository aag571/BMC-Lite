"""Linux 回环集成测试，验证慢客户端不影响采样。"""
import http.client
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="bmc-network-") as directory:
    root = pathlib.Path(directory)
    config = root / "sensors.conf"
    config.write_text("cpu mock 40 1 high 70 90 3 1 3 -\n")
    common = [binary, "--config", str(config), "--rules", str(root / "none"),
              "--sel", str(root / "sel"), "--log", str(root / "log"), "--interval-ms", "10"]
    def request(port, path):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
        connection.request("GET", path)
        response = connection.getresponse()
        body = response.read()
        assert response.status == 200, (response.status, body)
        assert response.getheader("Connection") == "close"
        assert int(response.getheader("Content-Length")) == len(body)
        connection.close()
        return body
    process = subprocess.Popen(common, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(.1)
        assert process.poll() is None
        links = [path.readlink().as_posix() for path in pathlib.Path(f"/proc/{process.pid}/fd").iterdir()]
        assert not any(link.startswith("socket:") for link in links), links
    finally:
        process.terminate()
        assert process.wait(timeout=3) == 0
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    process = subprocess.Popen(common + ["--http-port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    slow = None
    try:
        deadline = time.monotonic() + 3
        while True:
            try:
                initial = json.loads(request(port, "/healthz"))
                break
            except OSError:
                assert time.monotonic() < deadline
                time.sleep(.02)
        slow = socket.create_connection(("127.0.0.1", port), timeout=2)
        slow.sendall(b"GET / HTTP/1.1\r\nHost:")
        time.sleep(.1)
        later = json.loads(request(port, "/healthz"))
        assert later["samples"] > initial["samples"]
        assert json.loads(request(port, "/redfish/v1/"))["Id"] == "RootService"
        metrics = request(port, "/metrics").decode()
        assert 'bmc_requests_total{role="read",method="GET",status="200"}' in metrics
        assert 'bmc_request_bytes_total{role="read",direction="in"}' in metrics
        slow.settimeout(6)
        assert slow.recv(1) == b""
        malformed = socket.create_connection(("127.0.0.1", port), timeout=2)
        malformed.sendall(b"invalid\r\n\r\n")
        assert b"400 Bad Request" in malformed.recv(1024)
        malformed.close()
        time.sleep(.05)
        metrics = request(port, "/metrics").decode()
        assert 'reason="timeout"} 1' in metrics and 'reason="parse"} 1' in metrics, metrics
        clients = [socket.create_connection(("127.0.0.1", port), timeout=2) for _ in range(65)]
        time.sleep(.1)
        assert clients[-1].recv(1) == b"", "connection limit did not close the extra client"
        for client in clients:
            client.close()
        time.sleep(.1)
        metrics = request(port, "/metrics").decode()
        assert 'reason="limit"} 1' in metrics, metrics
    finally:
        if slow:
            slow.close()
        process.terminate()
        assert process.wait(timeout=3) == 0
    with socket.socket() as occupied:
        occupied.bind(("127.0.0.1", 0))
        occupied.listen()
        result = subprocess.run(common + ["--http-port", str(occupied.getsockname()[1]),
                                "--ticks", "5"], capture_output=True, timeout=3)
        assert result.returncode == 1 and b"HTTP unavailable" in result.stderr
print("default-off, routes, slow-client isolation, bind failure: passed")
