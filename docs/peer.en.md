# Peer heartbeat installation and use

Peers use a separate listener and token, disabled by default. They exchange liveness and configuration generations only. The module has no Worker, RecoveryPolicyEngine, Action or device reference. It does not elect a leader, fence hardware or fail over.

## Two local instances

From the repository root:

```sh
mkdir -p var/peer-demo
openssl rand -hex 32 > var/peer-demo/token
chmod 0640 var/peer-demo/token
# Terminal A:
./build/bmc-lite --config config/mock.conf --rules config/rules.conf \
  --log var/peer-demo/a.log --sel var/peer-demo/a.sel --http-port 8001 \
  --peer-address 127.0.0.1 --peer-port 9002 --peer-listen-port 9001 \
  --peer-token-file var/peer-demo/token --peer-interval-ms 1000 --peer-stale-ms 5000
# Terminal B:
./build/bmc-lite --config config/mock.conf --rules config/rules.conf \
  --log var/peer-demo/b.log --sel var/peer-demo/b.sel --http-port 8002 \
  --peer-address 127.0.0.1 --peer-port 9001 --peer-listen-port 9002 \
  --peer-token-file var/peer-demo/token --peer-interval-ms 1000 --peer-stale-ms 5000
# Terminal C:
curl -s http://127.0.0.1:8001/metrics | grep bmc_peer
```

Stop B: A reports `peer stale` after five seconds in SEL/log/EventBus and sets bmc_peer_stale=1. Restart B: A reports recovery. Send B SIGHUP to observe its new generation on A. The timer starts at startup even if no heartbeat has ever arrived. Repeated stale checks do not spam logs.

## Remote TLS deployment

Install both daemons using README.en.md and create certificates containing each server's actual IP/DNS SAN using control.en.md. Install a separate shared peer-token under /opt/bmc-lite/control on each machine, owned by bmc-lite, mode 0640. Do not reuse the control token. Copy A's public certificate to B as peer-ca.pem and B's certificate to A; private keys remain on their servers. With an internal CA, use the CA certificate instead. Both builds require BMC_TLS=ON.

Append to A's complete systemd ExecStart, when B is 192.168.124.128:

```text
--peer-bind 0.0.0.0 --peer-listen-port 9443 --peer-address 192.168.124.128 --peer-port 9443 --peer-token-file /opt/bmc-lite/control/peer-token --peer-ca /opt/bmc-lite/control/peer-ca.pem --peer-server-name 192.168.124.128 --peer-tls-cert /opt/bmc-lite/control/cert.pem --peer-tls-key /opt/bmc-lite/control/key.pem
```

On B, reverse the remote address/server-name to A's actual identity. Keep config/rules/sel/log arguments and add --http-port 8000 for metrics. Include ReadOnlyPaths=/opt/bmc-lite/control in the override, daemon-reload/restart and allow TCP 9443 between the two management hosts. Private keys mode 0600; tokens owned by the runtime user, exactly 0640, regular and not symlinks. Rotate credentials by restarting.

Nonloopback outgoing traffic requires a CA and verified server name; nonloopback listeners require a certificate/key. Wrong CA, hostname or token cannot refresh liveness. There is no insecure verification switch. Startup failure stops the peer listener, preserves sampling and yields exit 1. Only loopback may omit TLS.

## Protocol and bounds

GET /v1/heartbeat with Authorization: Bearer and X-BMC-Generation headers; 200 returns the local generation as decimal text plus newline. The token is checked before the route: missing or wrong credentials always return 401, and repeated failures return 429 after per-source rate limiting; a valid token with an unknown route, wrong method or bad generation returns 400. At most eight inbound connections, one outbound exchange, 8 KiB response buffer, 32..512 character tokens. Use a 64-character random hexadecimal token.

Connect/send/recv are nonblocking. Each exchange has an absolute min(5s, stale_ms) deadline. Retry interval is interval_ms (>=10ms); stale_ms must exceed it. Stop waits at most one 10ms thread sleep. Valid incoming requests or verified outgoing responses refresh liveness. Generation is observed, never automatically applied to local configuration.

Metrics: bmc_peer_stale, bmc_peer_generation, bmc_peer_heartbeats_total, bmc_peer_failures_total and network metrics with role="peer". Tune the timeout for actual network jitter.
