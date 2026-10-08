# Uplink telemetry

Disabled by default. An independent thread forwards sensor_state/configuration/service/recovery
events as newline-delimited JSON to an IPv4 TCP collector. It accepts no hardware commands.
Connect, send and disconnect detection are nonblocking; EventBus callbacks only encode and enqueue.

## Run

On a demonstration collector:

```sh
sudo apt-get install -y netcat-openbsd
nc -lk 9000 | tee telemetry.jsonl
```

On the daemon host (replace 127.0.0.1 with the collector IPv4 for separate hosts):

```sh
./build-release/bmc-lite --config config/mock.conf --rules config/rules.conf --sel var/sel.db --log var/faults.jsonl --http-port 8000 --uplink-address 127.0.0.1 --uplink-port 9000 --uplink-capacity 256
curl http://127.0.0.1:8000/healthz
curl http://127.0.0.1:8000/metrics | grep bmc_uplink
```

For systemd, append --uplink-address COLLECTOR_IP --uplink-port 9000 --uplink-capacity 256
to the existing ExecStart override, run sudo systemctl daemon-reload, then restart bmc-lite.
Source/release installation follows README; there are no additional library dependencies.
Address/port must be supplied together. Ports: 1..65535; queue: 1..4096, default 256.
DNS is not supported. Uplink uses plaintext TCP; the TLS option applies to control HTTPS only.
Use a trusted network or tunnel for telemetry.

## Framing and limits

```json
{"type": "sensor_state", "source": "cpu", "message": "critical", "sequence": 12, "time_ms": 1728000000000, "value": 95}
```

Sequence is the process-local EventBus sequence and resets on restart. time_ms is enqueue time.
Parse by newline, not recv boundaries. Embedded newlines are escaped. Events report transitions
and business activity, not every raw sample. Sequence gaps can indicate drops.

Delivery is best effort: no ACK, durable queue or replay guarantee. sent means a complete frame
was handed to the local socket, not persisted by the collector. Partial frames are dropped on
disconnect; collectors must discard an unterminated EOF fragment. This is a one-way protocol:
collector responses cause the connection to be closed.

Overflow drops the oldest waiting message. There is at most one additional in-flight frame,
limited to 8 KiB. Source/message combined are limited to 4 KiB, encoded frames to 8 KiB.
Buffer memory is bounded by approximately (capacity + 1) × 8 KiB plus object overhead.
Connect/send deadlines are five seconds. Reconnect backoff starts at 250 ms and doubles to
30 seconds, resetting after successful message delivery to the socket.
Shutdown stops inbound requests and Worker, drains EventBus, then stops uplink. Unsent frames
are counted as dropped; offline collectors cannot delay shutdown. Local SEL evidence is retained.

## Metrics and tests

When uplink and read-only HTTP are enabled, /metrics adds:

| Metric | Meaning |
|---|---|
| bmc_uplink_queued | Waiting frames, excluding one in-flight frame |
| bmc_uplink_dropped_total | Overflow, oversized, failed/timed-out and shutdown drops |
| bmc_uplink_sent_total | Complete frames handed to the local socket |
| bmc_uplink_bytes_total | Bytes sent, including partial failed frames |
| bmc_uplink_connect_attempts_total | Connection attempts |
| bmc_uplink_connected | Last observed connection state, 1/0 |

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R 'Uplink|uplink_runtime' --output-on-failure
```

C++ FakeSocketIo tests cover partial sends/EAGAIN, 50,000 enqueues, deadlines, backoff,
idle disconnects and all event types. Real TCP integration tests cover missing/stalled collectors,
bounded queues, sampling progress, reconnection and JSONL frames.
