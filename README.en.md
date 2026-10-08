# BMC-Lite — Installation and User Guide

[中文手册](README.md)

BMC-Lite is a Linux C++20 hardware monitoring resume project. The daemon samples devices, confirms fault states, evaluates rules, schedules recovery tasks, and writes JSONL logs and persistent events. A separate Python utility provides read-only HTTP resources and Prometheus metrics.

## 1. Choose your installation path

Use **source installation** when developing or running tests. Use the **release archive** when deploying without a compiler. The prebuilt binary is Linux x86_64; its glibc and libstdc++ requirements depend on the build machine. Build from source if the target reports a missing GLIBC/GLIBCXX version. The current release is built on the development VM; Ubuntu 24.04 x86_64 is the recommended target.

The target VM in this example is `192.168.124.128`. Replace `user` with your actual SSH account. GitHub URL and tag placeholders must be replaced with your published repository values; no GitHub release is assumed to exist.

## 2. Prepare a fresh Ubuntu VM

Run these commands **inside the target VM**:

```sh
uname -m
cat /etc/os-release
sudo apt-get update
sudo apt-get install -y ca-certificates curl python3 tar libstdc++6
```

For source builds, also install:

```sh
sudo apt-get install -y git build-essential cmake
g++ --version
cmake --version
```

GCC 11+ and CMake 3.20+ are required. If you need SSH access, install `openssh-server`, start its service, and verify that the VM is reachable before copying files.

## 3. Clone, compile, and test from source

```sh
mkdir -p ~/src
cd ~/src
git clone <GITHUB_REPOSITORY_URL> bmc-lite
cd bmc-lite
git fetch --tags
# Optional: git checkout <RELEASE_TAG>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

The first test build downloads GoogleTest 1.15.2. An offline production build can disable tests. The tests are not all Python:

| Component | Implementation | Purpose |
|---|---|---|
| `tests/core_test.cpp` / `build/bmc_tests` | C++ / GoogleTest | State machine, rules, recovery, devices, worker, event bus, persistence |
| `tests/stress_test.cpp` / `build/bmc_stress` | C++ | Concurrent producers and bounded queue accounting |
| `tests/runtime_test.py` | Python | Process startup, SIGHUP reload, invalid configuration, SIGTERM |
| `tests/fault_injection_test.py` | Python | Daemon behavior with missing inputs and failed writes |
| `tests/metrics_test.py` | Python | Python management-tool formatting |
| `tests/linux_io_fake_test.py` | Python | Basic OS file-descriptor checks; unrelated to the C++ `FakeLinuxIo` |

Run C++ tests independently:

```sh
./build/bmc_tests
./build/bmc_tests --gtest_filter='Engine.*:Rules.*:Recovery.*'
./build/bmc_stress
```

`LinuxIo` (`include/bmc/linux_io.hpp`) is the single injection point for system calls. `PosixLinuxIo` forwards to the real `open`/`ioctl`/`read`/`write`/`close`, and the `FakeLinuxIo` defined in `tests/core_test.cpp` scripts ioctl replies and captures writes, so the I2C decode path and the PWM write path are covered without `/dev/i2c-*` or `/dev/gpiochip*`. Coverage stops short of the GPIO line descriptor: `GPIO_V2_GET_LINE_IOCTL` returns a kernel-assigned fd that a fake cannot provide, so GPIO tests assert request construction (offsets, flags, the `fcntl` failure) rather than a live line. `tests/linux_io_fake_test.py` is unrelated to `FakeLinuxIo` despite the similar name.

## 4. First foreground run

From the repository root:

```sh
./build/bmc-lite --check-config --config config/mock.conf --rules config/rules.conf
mkdir -p var
./build/bmc-lite --config config/mock.conf --rules config/rules.conf \
  --sel var/sel.db --log var/faults.jsonl --interval-ms 20 --ticks 40
tail -n 20 var/faults.jsonl
python3 tools/bmc_manage.py --sel var/sel.db sel-info
```

Expect warning, critical, unavailable, and recovery transitions. Exit code 0 indicates a normal stop. Omit `--ticks` for continuous operation; press Ctrl+C to stop. Actions are simulated unless `--enable-actions` is present.

## 5. Source Release installation

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --parallel 2
./build-release/bmc-lite --help
sudo bash deploy/install.sh
sudo systemctl enable --now bmc-lite.service
systemctl is-active bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

Installation creates the service account, installs the daemon/configuration/management tool under `/opt/bmc-lite`, and creates `/var/log/bmc-lite`. The first configuration is mock mode despite the filename `hardware.conf`. Existing configuration and rules are preserved. The installer refuses to replace a running daemon.

## 6. Create or install a release archive

On the build machine, from the project root:

```sh
bash tools/package-release.sh
sha256sum -c bmc-lite-release.tar.gz.sha256
tar -tzf bmc-lite-release.tar.gz | head
scp bmc-lite-release.tar.gz bmc-lite-release.tar.gz.sha256 user@192.168.124.128:/tmp/
```

Alternatively, download both files from your actual GitHub Releases page. On the target:

```sh
ssh user@192.168.124.128
cd /tmp
sha256sum -c bmc-lite-release.tar.gz.sha256
tar -xzf bmc-lite-release.tar.gz
cd bmc-lite-release
ldd build-release/bmc-lite
./build-release/bmc-lite --help
sudo bash deploy/install.sh
sudo systemctl enable --now bmc-lite.service
systemctl is-active bmc-lite.service
```

Use a fresh extraction directory when installing a different version. Do not continue if checksum verification fails or `ldd` reports a missing library. The corrected package layout includes `build-release/bmc-lite`, exactly as expected by the installer.

## 7. Everyday operation

```sh
sudo systemctl status bmc-lite.service --no-pager
sudo journalctl -u bmc-lite.service -f
sudo tail -n 30 /var/log/bmc-lite/faults.jsonl
sudo -u bmc-lite python3 /opt/bmc-lite/tools/bmc_manage.py --sel /var/log/bmc-lite/sel.db sel-info
sudo -u bmc-lite python3 /opt/bmc-lite/tools/bmc_manage.py --sel /var/log/bmc-lite/sel.db sel-list
```

Use `sel-get <id>` for an existing ID returned by `sel-list`; IDs are not guaranteed to start at 1 in an existing installation.

Edit and validate configuration:

```sh
sudo cp /opt/bmc-lite/config/hardware.conf /opt/bmc-lite/config/hardware.conf.bak
sudo nano /opt/bmc-lite/config/hardware.conf
sudo nano /opt/bmc-lite/config/rules.conf
sudo -u bmc-lite /opt/bmc-lite/bmc-lite --check-config \
  --config /opt/bmc-lite/config/hardware.conf --rules /opt/bmc-lite/config/rules.conf
sudo systemctl kill --kill-whom=main --signal=HUP bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

Look for `validated generation`; a rejected reload leaves the previous in-memory configuration active. Restore the backup on disk if validation fails. Hardware availability is not validated by `--check-config`.

## 8. Configuration reference

Sensor fields: `id backend path scale direction warning critical hysteresis debounce failure_limit action_path`.

`high` means larger values are worse; `low` means smaller values are worse. `scale` converts raw units. `debounce` confirms threshold changes, `failure_limit` confirms read failures, and `hysteresis` prevents recovery oscillation. `-` disables PWM for the sensor.

```text
cpu_temp mock 40,95,95,95,err,err,err,40,40,40 1 high 70 90 3 3 3 -
cpu_temp sysfs /sys/class/hwmon/hwmon0/temp1_input 0.001 high 70 90 3 3 3 -
gpio_fault gpio /dev/gpiochip0,23,active-low 1 high 0.5 1 0 1 1 -
```

Rules: `id sensor state confirmations clear_confirmations action`. States are warning/critical/unavailable; `*` matches all sensors independently. Actions are `increase_fan` and `inspect_device`. Confirmation counts apply to sampled state after the threshold engine has applied its own debounce. `inspect_device` acknowledges a request; it is not automatic device diagnosis.

Do not assume that hwmon numbering is stable across boots. Generic I2C reads are unsigned SMBus words, not a driver for every temperature chip. GPIO v2 supports level polling; `--gpio` is a separate legacy sysfs edge input.

## 9. HTTP and metrics after installation

The monitor systemd service does **not** automatically start the HTTP utility. In a second terminal:

```sh
sudo -u bmc-lite python3 /opt/bmc-lite/tools/bmc_manage.py \
  --sel /var/log/bmc-lite/sel.db serve --port 8000
```

In another terminal:

```sh
curl -f http://127.0.0.1:8000/redfish/v1/
curl -f http://127.0.0.1:8000/redfish/v1/Managers/BMC/LogServices/SEL/Entries
curl -f http://127.0.0.1:8000/metrics
```

Keep the HTTP terminal running; Ctrl+C stops it. To access from your workstation, open a third terminal on the workstation:

```sh
ssh -N -L 18000:127.0.0.1:8000 user@192.168.124.128
curl -f http://127.0.0.1:18000/metrics
```

Metrics report recorded event state and value, not every live sensor sample. HTTP is loopback-only, with bounded concurrent handlers and connection timeout. It has no TLS/authentication and is not standard Redfish or IPMI.

## 10. Controlled demonstration

Keep mock mode, change its sequence from normal values to sustained critical values, validate, and reload. Observe a critical transition, rule activation, simulated recovery, and SEL records. Change the sequence back to normal and reload to observe recovery. Test invalid configuration using the integration tests instead of modifying the installed service unnecessarily:

```sh
ctest --test-dir build -R 'runtime_reload|fault_injection' --output-on-failure
```

## 11. Upgrade, rollback, and troubleshooting

Back up the installed directory and stop the service through systemd before running the installer. The installer preserves configuration. Start the service, inspect journal output, and restore the backup if necessary. Never replace the executable while it is running.

For `GLIBCXX` errors, compile on the target. For unavailable sensors, check device paths/permissions. For SEL access errors, run the utility as `bmc-lite` because its log directory is private. For refused HTTP connections, check that the separate utility is running. For inactive services, inspect `journalctl -u bmc-lite.service -b` and `systemctl cat bmc-lite.service`.

## 12. Validation and scope

```sh
bash tools/validate.sh normal
bash tools/validate.sh sanitize
bash tools/validate.sh stress
sudo apt-get install -y valgrind
bash tools/validate.sh valgrind
```

This is an educational resume project, not complete BMC firmware. SEL is a custom text format without power-loss durability guarantees. Real I2C/GPIO electrical behavior needs hardware or an appropriate QEMU model. Python OS descriptor tests are not mocked C++ ioctl tests. See `docs/code-guide.md` for the source reading order.
