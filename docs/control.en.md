# Control service installation and usage

Control is disabled by default. A port and valid token file are both required. Invalid credentials,
certificates or binding make the daemon exit immediately with a nonzero status. A failed control
listener or audit at runtime also exits nonzero so systemd can restart it. Read-only HTTP uses a
separate port and its bind failure still allows monitoring to continue.

## Source installation on Ubuntu 24.04

```sh
git clone git@github.com:aag571/BMC-Lite.git
cd BMC-Lite
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev openssl curl python3
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBMC_TLS=ON
cmake --build build-release --parallel 2
sudo bash deploy/install.sh
```

BMC_TLS defaults to OFF; OpenSSL 3 is needed only for HTTPS. Without TLS, control may bind
only to an IPv4 loopback address. For a release archive built with TLS:

```sh
sha256sum -c bmc-lite-release.tar.gz.sha256
tar -xzf bmc-lite-release.tar.gz
cd bmc-lite-release
sudo apt-get install -y libssl3t64 openssl curl
sudo bash deploy/install.sh
```

The installer checks missing dynamic libraries and creates /opt/bmc-lite/control.
It does not generate or replace credentials.

## Token and demonstration certificate

```sh
sudo install -d -o bmc-lite -g bmc-lite -m 0750 /opt/bmc-lite/control
openssl rand -hex 32 > /tmp/bmc-control-token
sudo install -o bmc-lite -g bmc-lite -m 0640 /tmp/bmc-control-token /opt/bmc-lite/control/token
rm /tmp/bmc-control-token
openssl req -x509 -newkey rsa:2048 -nodes -days 30 -keyout /tmp/bmc-key.pem -out /tmp/bmc-cert.pem -subj /CN=bmc-lite -addext 'subjectAltName=IP:192.168.124.128,DNS:localhost'
sudo install -o bmc-lite -g bmc-lite -m 0600 /tmp/bmc-key.pem /opt/bmc-lite/control/key.pem
sudo install -o root -g bmc-lite -m 0640 /tmp/bmc-cert.pem /opt/bmc-lite/control/cert.pem
rm /tmp/bmc-key.pem
```

Tokens must be regular files owned by the process user, with mode exactly 0640. Symlinks are
rejected. Content must have at least 32 printable non-space characters; one final newline is allowed.
For manual execution, set ownership to the account running the binary. Never pass tokens on the CLI.

Copy cert.pem to the client as a trust anchor. Keep the private key on the server. In managed
networks use an internal CA certificate. Clients verify both chain and hostname/IP with --cacert;
do not use curl -k. This is server TLS plus Bearer authentication, not mTLS.
Restart after certificate/token rotation.

## Enable systemd

Run sudo systemctl edit bmc-lite and enter:

```ini
[Service]
ExecStart=
ExecStart=/opt/bmc-lite/bmc-lite --config /opt/bmc-lite/config/hardware.conf --rules /opt/bmc-lite/config/rules.conf --sel /var/log/bmc-lite/sel.db --log /var/log/bmc-lite/faults.jsonl --http-port 8000 --control-port 8443 --control-bind 0.0.0.0 --control-token-file /opt/bmc-lite/control/token --control-tls-cert /opt/bmc-lite/control/cert.pem --control-tls-key /opt/bmc-lite/control/key.pem
ReadOnlyPaths=/opt/bmc-lite/control
```

```sh
sudo systemctl daemon-reload
sudo systemctl restart bmc-lite
sudo systemctl status bmc-lite --no-pager
sudo journalctl -u bmc-lite -n 50 --no-pager
ss -ltn | grep -E ':8000|:8443'
curl http://127.0.0.1:8000/healthz
```

Read-only HTTP stays on loopback. To expose it explicitly, add --http-bind 0.0.0.0 --http-allow-remote.
Control allows eight connections, with read/write/handshake deadlines. Failed authentication is
limited by source IP, so changing source ports cannot bypass it. Non-loopback plaintext is rejected.

## Requests and results

Create control.curl.conf on the client:

```text
header = "Authorization: Bearer REPLACE_WITH_TOKEN"
cacert = "/client/path/cert.pem"
```

```sh
chmod 0600 control.curl.conf
curl --config control.curl.conf https://192.168.124.128:8443/v1/config/generation
curl --config control.curl.conf -H 'Content-Type: application/json' --data '{"sensor":"cpu","action":"inspect_device"}' https://192.168.124.128:8443/v1/actions/inspect
curl --config control.curl.conf -H 'Content-Type: application/json' --data '{"sensor":"cpu","action":"increase_fan"}' https://192.168.124.128:8443/v1/actions/fan
```

The sensor must exist in current configuration. Fan actions also need action_path. Bodies accept
exactly sensor/action strings; paths, duplicate keys, additional fields and string escapes are rejected.
Paths come from a locked configuration snapshot. 202 means queued, not completed. Worker invokes
RecoveryPolicyEngine; its 30-second cooldown prevents duplicate writes. Actions are simulated by
default. Real PWM needs --enable-actions and device permissions; it writes 255 to the configured node.

Responses: 200 query success; 202 queued; 400 invalid request; 401 bad credentials; 429 rate limit;
503 queue full or unavailable audit persistence. SEL stores action/sensor/outcome/peer/request_id.
Outcomes include requested/accepted/rejected/unauthorized/rate-limited. A valid action is audited
as requested before queueing and as accepted only after successful queueing. No action runs until
the accepted audit is durable. Correlate validation and execution
by request_id; detail can be completed, failed, cooldown, already running, concurrency limit or
state capacity. Accepted validation can be followed by
queue rejection or action failure.

## Audit, reload and tests

```sh
sudo grep 'control' /var/log/bmc-lite/sel.db | tail -20
sudo tail -20 /var/log/bmc-lite/faults.jsonl
sudo systemctl kill --kill-whom=main --signal=HUP bmc-lite
curl --config control.curl.conf https://192.168.124.128:8443/v1/config/generation
cmake -S . -B build-tls -DBMC_TLS=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tls --parallel 2
ctest --test-dir build-tls --output-on-failure
```

Successful sensor/rule validation updates generation and the control snapshot; failed reloads
retain old configuration. Queued tasks retain value copies, never sensor pointers. C++ tests cover
permissions, symlinks, protocol rejection and cooldown. Process tests cover HTTP/HTTPS, invalid
certificate chain/hostname rejection, temporary PWM files, reload, eight connections and SEL audit.
