# BMC-Lite: Installation and User Guide

> 该文档由AI翻译中文文档

[中文手册](README.md)

## 1. Prepare Linux

Ubuntu 24.04 x86_64 is recommended. Run these commands inside the target VM; replace `user` with your actual SSH account.

```sh
uname -m
cat /etc/os-release
sudo apt-get update
sudo apt-get install -y ca-certificates curl python3 tar libstdc++6 libssl3t64 openssl
# Source builds also need:
sudo apt-get install -y git build-essential cmake libssl-dev
g++ --version
cmake --version
```

GCC 11+, CMake 3.20+ and OpenSSL 3 for TLS are required. Simulation needs no hardware. The release build is Linux x86_64 and depends on the build machine's glibc/libstdc++; compile on the target when its libraries are too old.

## 2. Clone, compile and test from GitHub

```sh
mkdir -p ~/src
cd ~/src
git clone https://github.com/aag571/BMC-Lite.git
cd BMC-Lite
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBMC_TLS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/bmc_tests
./build/bmc_tests --gtest_filter='Engine.*:Rules.*:Recovery.*:FakeIo.*'
./build/bmc_stress
```

The first test build downloads GoogleTest 1.15.2; an offline build can use `-DBUILD_TESTING=OFF`. When updating an existing clone, save your own changes first, then `git pull --ff-only` and rebuild.

The test suite is mostly C++:

- `core_test.cpp` covers the domain and FakeLinuxIo.
- `network/control/uplink/peer_test.cpp` cover the network state machines.
- `runtime_management_test.cpp` verifies degradation and in-memory state trimming.
- `stress_test.cpp` verifies the concurrent queue.
- Python scripts start the real process and test HTTP/TLS, signals, hot reload and fault injection.
- The GPIO fake returns a real placeholder descriptor, verifying line values, failures, CLOEXEC and closure.
- Electrical behavior requires hardware.

## 3. First foreground run

From the repository root:

```sh
./build/bmc-lite --check-config --config config/mock.conf --rules config/rules.conf
mkdir -p var
./build/bmc-lite --config config/mock.conf --rules config/rules.conf \
  --sel var/sel.db --log var/faults.jsonl --interval-ms 20 --ticks 40
tail -n 20 var/faults.jsonl
python3 tools/bmc_manage.py --sel var/sel.db sel-info
python3 tools/bmc_manage.py --sel var/sel.db sel-list
```

- The mock sequence produces state transitions, rule and recovery records.
- 0 means a normal exit; drop `--ticks` to run continuously and press Ctrl+C to stop.
- Without `--enable-actions` fan actions are dry-run.
- With no rules configured a warning is recorded and threshold monitoring continues.

## 4. Source Release installation

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBMC_TLS=ON
cmake --build build-release --parallel 2
./build-release/bmc-lite --help
sudo bash deploy/install.sh
sudo systemctl enable --now bmc-lite.service
systemctl is-active bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

- Installation creates the `bmc-lite` account and puts the program, configuration and tools under `/opt/bmc-lite`, with logs under `/var/log/bmc-lite`.
- The first `hardware.conf` is a runnable mock configuration; existing configuration and rules are preserved. The installer refuses to replace a running service, and systemd does not enable networking by default.

## 5. Release archive deployment

On the build machine, package and send to the target:

```sh
BMC_TLS=ON bash tools/package-release.sh
sha256sum -c bmc-lite-release.tar.gz.sha256
tar -tzf bmc-lite-release.tar.gz | head
scp bmc-lite-release.tar.gz bmc-lite-release.tar.gz.sha256 user@192.168.124.128:/tmp/
```

- If you download directly from Releases, both files must match. On the target:

```sh
ssh user@192.168.124.128
cd /tmp
sha256sum -c bmc-lite-release.tar.gz.sha256
mkdir -p ~/bmc-install
tar -xzf bmc-lite-release.tar.gz -C ~/bmc-install
cd ~/bmc-install/bmc-lite-release
ldd build-release/bmc-lite
./build-release/bmc-lite --help
sudo bash deploy/install.sh
sudo systemctl enable --now bmc-lite.service
systemctl is-active bmc-lite.service
```

- Do not install if checksum verification fails; resolve `not found` entries from `ldd` first.
- The Ubuntu 24.04 TLS package needs `libssl3t64`; the target needs neither a compiler nor GoogleTest.
- Use a fresh extraction directory when upgrading, so old files are not mixed in.

## 6. Inspection and hot reload after deployment

```sh
sudo systemctl status bmc-lite.service --no-pager
sudo journalctl -u bmc-lite.service -f
sudo tail -n 30 /var/log/bmc-lite/faults.jsonl
sudo -u bmc-lite python3 /opt/bmc-lite/tools/bmc_manage.py --sel /var/log/bmc-lite/sel.db sel-info
sudo -u bmc-lite python3 /opt/bmc-lite/tools/bmc_manage.py --sel /var/log/bmc-lite/sel.db sel-list
```

- `sel-get ID` queries a real ID returned by `sel-list`; an existing installation does not necessarily start at 1.
- The log directory is private; read it as the service account or root.

```sh
sudo cp /opt/bmc-lite/config/hardware.conf /opt/bmc-lite/config/hardware.conf.bak
sudo nano /opt/bmc-lite/config/hardware.conf
sudo nano /opt/bmc-lite/config/rules.conf
sudo -u bmc-lite /opt/bmc-lite/bmc-lite --check-config \
  --config /opt/bmc-lite/config/hardware.conf --rules /opt/bmc-lite/config/rules.conf
sudo systemctl kill --kill-whom=main --signal=HUP bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

- `validated generation` means the reload took effect.
- A failed reload keeps the previous in-memory configuration; restore the disk file from the backup.
- `--check-config` validates syntax and policy, not hardware availability.
- A valid rule reload preserves confirmation state, and state for removed sensors is cleaned up.

## 7. Configuration and hardware

Each line: `id backend path scale direction warning critical hysteresis debounce failure_limit action_path [calibration]`.

```text
cpu_temp mock 40,95,95,95,err,err,err,40,40,40 1 high 70 90 3 3 3 -
cpu_temp sysfs /sys/class/hwmon/hwmon0/temp1_input 0.001 high 70 90 3 3 3 -
gpio_fault gpio /dev/gpiochip0,23,active-low 1 high 0.5 1 0 1 1 -
```

- `high` means larger values are worse, `low` the opposite.
- scale converts units.
- debounce confirms a state.
- failure_limit confirms read failures.
- hysteresis controls recovery lag.
- `-` means no PWM.
- The optional `gain[:offset][;raw=value;...]` calibration supports linear correction and piecewise interpolation.

Rules: `id sensor state confirmations clear_confirmations action`. States are warning/critical/unavailable; actions are increase_fan/inspect_device. `*` counts each sensor independently and confirms on every sample. inspect only records an inspection request.

## 8. Enable daemon HTTP

Run `sudo systemctl edit bmc-lite.service` and fill in:

```ini
[Service]
ExecStart=
ExecStart=/opt/bmc-lite/bmc-lite --config /opt/bmc-lite/config/hardware.conf --rules /opt/bmc-lite/config/rules.conf --sel /var/log/bmc-lite/sel.db --log /var/log/bmc-lite/faults.jsonl --http-port 8000
```

```sh
sudo systemctl daemon-reload
sudo systemctl restart bmc-lite.service
curl -f http://127.0.0.1:8000/healthz
curl -f http://127.0.0.1:8000/redfish/v1/
curl -f http://127.0.0.1:8000/redfish/v1/Managers/BMC/LogServices/SEL/Entries
curl -f http://127.0.0.1:8000/metrics
```

- The `samples` field from healthz keeps increasing.
- Metrics include recorded sensor states/values, active network connections, limit/timeout/parse rejections, requests by role/method/status, byte counters, and extension metrics from enabled modules.
- This is not full standard Redfish/IPMI, and it does not emit every live sample.

Workstation forwarding: `ssh -N -L 18000:127.0.0.1:8000 user@192.168.124.128`, then run `curl http://127.0.0.1:18000/metrics` in another terminal. The Python `bmc_manage.py ... serve --port 8000` can still read the SEL independently, but it cannot share the port with the daemon and has no daemon network metrics.

## 9. Validation and troubleshooting

```sh
sudo apt-get install -y valgrind strace
bash tools/validate.sh normal
bash tools/validate.sh sanitize
bash tools/validate.sh stress
bash tools/validate.sh valgrind
bash tools/validate.sh tsan
# If GCC TSan reports unexpected memory mapping and setarch is permitted:
BMC_TSAN_NO_ASLR=1 bash tools/validate.sh tsan
bash tools/bench-writes.sh 50000
```

- ASan/UBSan and TSan use different directories.
- setarch only affects this test process and its children, not global configuration.

Common problems and what to check:

- For a service failure, look at `journalctl -u bmc-lite.service -b`.
- For `unavailable` sensors, check paths and permissions.
- For `GLIBCXX` errors, compile on the target.
- For HTTP, check whether a port was enabled.
- For control, check the token owner/mode 0640 and the certificate.
- A failed network start keeps monitoring running and eventually exits with status 1.

## 10. Upgrade and rollback

```sh
sudo systemctl stop bmc-lite.service
sudo cp -a /opt/bmc-lite /opt/bmc-lite.backup
sudo cp -a /var/log/bmc-lite /var/log/bmc-lite.backup
# In the new release extraction directory or the source root:
sudo bash deploy/install.sh
sudo systemctl start bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

- The installer preserves configuration; systemd overrides keep working.
- To roll back, stop the service first, restore the backed-up program and configuration, then start it.
- Never overwrite the binary while it is running.
