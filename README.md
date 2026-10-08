# BMC-Lite：从零安装与使用

[English guide](README.en.md)

Linux C++20 硬件监测与故障恢复简历项目。采样、去抖/迟滞、规则、线程池、事件总线、日志和 SEL 组成闭环。只读 HTTP、认证控制、上行遥测和实例心跳默认关闭。[验收记录](docs/completion.md)列出功能与验证范围，[代码指南](docs/code-guide.md)解释对象和数据流。

## 1. 准备 Linux

推荐 Ubuntu 24.04 x86_64。以下命令在目标虚拟机内执行；部署示例为 `192.168.124.128`，SSH 的 `user` 替换为实际用户。

```sh
uname -m
cat /etc/os-release
sudo apt-get update
sudo apt-get install -y ca-certificates curl python3 tar libstdc++6 libssl3t64 openssl
# 源码编译另需：
sudo apt-get install -y git build-essential cmake libssl-dev
g++ --version
cmake --version
```

要求 GCC 11+、CMake 3.20+，TLS 使用 OpenSSL 3。模拟不需要硬件。Release 是 Linux x86_64，依赖构建机的 glibc/libstdc++；目标库太旧时在目标机编译。

## 2. 从 GitHub 拉取、编译、测试

公开仓库用 HTTPS，无需 GitHub 密码：

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

首次测试构建会下载 GoogleTest 1.15.2；离线运行版可用 `-DBUILD_TESTING=OFF`。已有仓库更新时先保存自己的改动，再 `git pull --ff-only`，重新编译。

测试主体是 C++：`core_test.cpp` 覆盖领域与 FakeLinuxIo，`network/control/uplink/peer_test.cpp` 覆盖网络状态机，`runtime_management_test.cpp` 验证降级和内存状态裁剪，`stress_test.cpp` 验证并发队列。Python 脚本启动真实进程，测试 HTTP/TLS、信号、热加载和故障注入；`linux_io_fake_test.py` 仅是 Python OS 描述符检查，与 C++ Fake 无关。GPIO Fake 返回真实占位 fd，验证线值、失败、CLOEXEC 和关闭；电气行为需实机。

## 3. 第一次前台运行

在仓库根目录执行：

```sh
./build/bmc-lite --check-config --config config/mock.conf --rules config/rules.conf
mkdir -p var
./build/bmc-lite --config config/mock.conf --rules config/rules.conf \
  --sel var/sel.db --log var/faults.jsonl --interval-ms 20 --ticks 40
tail -n 20 var/faults.jsonl
python3 tools/bmc_manage.py --sel var/sel.db sel-info
python3 tools/bmc_manage.py --sel var/sel.db sel-list
```

模拟序列产生状态变化、规则和恢复记录。0 表示正常退出，去掉 `--ticks` 后持续运行，Ctrl+C 停止。没有 `--enable-actions` 时风扇动作也是 dry-run；没有规则时记录警告，阈值监控继续。

## 4. 源码 Release 安装

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBMC_TLS=ON
cmake --build build-release --parallel 2
./build-release/bmc-lite --help
sudo bash deploy/install.sh
sudo systemctl enable --now bmc-lite.service
systemctl is-active bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

安装创建 `bmc-lite` 账户，程序/配置/工具放到 `/opt/bmc-lite`，日志放到 `/var/log/bmc-lite`。首次 `hardware.conf` 是可直接运行的模拟配置；已有配置和规则保留。安装器拒绝替换运行中的服务，systemd 默认不开网络。

## 5. Release 压缩包部署

在开发机制作并发送到目标机：

```sh
BMC_TLS=ON bash tools/package-release.sh
sha256sum -c bmc-lite-release.tar.gz.sha256
tar -tzf bmc-lite-release.tar.gz | head
scp bmc-lite-release.tar.gz bmc-lite-release.tar.gz.sha256 user@192.168.124.128:/tmp/
```

当前交付压缩包和校验文件，没有假定 GitHub Releases 已上传附件。若从 Releases 下载，两个文件必须匹配。在目标机执行：

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

校验失败不要安装，`ldd` 的 `not found` 要先解决。Ubuntu 24.04 TLS 包需要 `libssl3t64`；目标不需要编译器和 GoogleTest。升级时用新解压目录，避免混入旧文件。

## 6. 部署后查看与热加载

```sh
sudo systemctl status bmc-lite.service --no-pager
sudo journalctl -u bmc-lite.service -f
sudo tail -n 30 /var/log/bmc-lite/faults.jsonl
sudo -u bmc-lite python3 /opt/bmc-lite/tools/bmc_manage.py --sel /var/log/bmc-lite/sel.db sel-info
sudo -u bmc-lite python3 /opt/bmc-lite/tools/bmc_manage.py --sel /var/log/bmc-lite/sel.db sel-list
```

`sel-get ID` 查询 `sel-list` 给出的真实 ID，已有安装不一定从 1 开始。日志目录私有，以服务用户或 root 读取。

```sh
sudo cp /opt/bmc-lite/config/hardware.conf /opt/bmc-lite/config/hardware.conf.bak
sudo nano /opt/bmc-lite/config/hardware.conf
sudo nano /opt/bmc-lite/config/rules.conf
sudo -u bmc-lite /opt/bmc-lite/bmc-lite --check-config \
  --config /opt/bmc-lite/config/hardware.conf --rules /opt/bmc-lite/config/rules.conf
sudo systemctl kill --kill-whom=main --signal=HUP bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

`validated generation` 表示生效。失败保留旧内存配置，磁盘文件应恢复备份。`--check-config` 校验语法和策略，不验证硬件可用性。有效规则重载保留确认状态，已删除传感器状态会清理。

## 7. 配置与硬件

每行：`id backend path scale direction warning critical hysteresis debounce failure_limit action_path [calibration]`。

```text
cpu_temp mock 40,95,95,95,err,err,err,40,40,40 1 high 70 90 3 3 3 -
cpu_temp sysfs /sys/class/hwmon/hwmon0/temp1_input 0.001 high 70 90 3 3 3 -
gpio_fault gpio /dev/gpiochip0,23,active-low 1 high 0.5 1 0 1 1 -
```

`high` 越大越危险，`low` 相反；scale 换算单位；debounce 确认状态；failure_limit 确认读取失败；hysteresis 控制恢复迟滞；`-` 无 PWM。可选 `gain[:offset][;raw=value;...]` 标定支持线性修正与分段插值。

规则格式：`id sensor state confirmations clear_confirmations action`。状态 warning/critical/unavailable，动作 increase_fan/inspect_device；`*` 各传感器独立计数，按每次采样确认。inspect 只记录检查请求。

GPIO v2 后端轮询线值；`--gpio` 是独立的旧 sysfs 边沿入口。I2C 专用型号和未核实项见 [芯片说明](docs/chips.md)。hwmon 编号可能变化；实机需要设备权限。systemd 默认禁止 sysfs 写入，真实 PWM 需为具体配置节点加 `ReadWritePaths` 并启用 `--enable-actions`，见 [控制指南](docs/control.md)。

## 8. 启用 daemon HTTP

执行 `sudo systemctl edit bmc-lite.service`，填入：

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

healthz 的 samples 持续增加。指标包含记录中的传感器状态/数值、网络活跃连接、limit/timeout/parse 拒绝、按角色/方法/状态的请求数、字节计数，以及启用模块的扩展指标。不是完整标准 Redfish/IPMI，也不输出每次实时采样。

工作站转发：`ssh -N -L 18000:127.0.0.1:8000 user@192.168.124.128`，另开终端运行 `curl http://127.0.0.1:18000/metrics`。Python `bmc_manage.py ... serve --port 8000` 仍可独立读取 SEL，但不能与 daemon 占用同端口，且没有 daemon 网络指标。

## 9. 更多功能与操作

- [控制指南](docs/control.md)：0640 令牌、TLS、systemd、curl 和 SEL 审计。202 只表示排队，执行结果查审计。
- [上行指南](docs/uplink.md)：JSONL 采集、有界队列、重连和指标；无 ACK/持久重放，满时丢最旧。
- [心跳指南](docs/peer.md)：独立认证端口、双向代次、陈旧/恢复和远程证书校验；不选主、不写硬件。
- [运行可靠性](docs/runtime-reliability.md)：日志降级上报、规则状态裁剪。

多个功能要将参数合并为同一条 ExecStart，保留 config/rules/sel/log。入门演示保持 mock，修改序列为持续 critical，校验并重载，观察规则/恢复/SEL，再恢复正常值。

## 10. 验证和排错

```sh
sudo apt-get install -y valgrind strace
bash tools/validate.sh normal
bash tools/validate.sh sanitize
bash tools/validate.sh stress
bash tools/validate.sh valgrind
bash tools/validate.sh tsan
# GCC TSan 报 unexpected memory mapping 且系统允许 setarch 时：
BMC_TSAN_NO_ASLR=1 bash tools/validate.sh tsan
bash tools/bench-writes.sh 50000
```

ASan/UBSan 与 TSan 用不同目录。setarch 仅改变本次测试进程及子进程，不改全局配置。[验收记录](docs/completion.md)与[性能基准](docs/benchmarks.md)列出实际结果。

服务失败看 `journalctl -u bmc-lite.service -b`；unavailable 检查路径/权限；GLIBCXX 错误在目标机编译；HTTP 检查是否启用端口；控制检查令牌属主/0640 和证书。网络启动失败继续监控并最终退出 1。

## 11. 升级和回滚

```sh
sudo systemctl stop bmc-lite.service
sudo cp -a /opt/bmc-lite /opt/bmc-lite.backup
sudo cp -a /var/log/bmc-lite /var/log/bmc-lite.backup
# 在新 release 解压目录或源码根目录：
sudo bash deploy/install.sh
sudo systemctl start bmc-lite.service
sudo journalctl -u bmc-lite.service -n 30 --no-pager
```

安装器保留配置；systemd override 继续生效。回滚先停服务，恢复备份的程序和配置，再启动。不要运行中覆盖二进制。

## 12. 边界

本项目不是完整 BMC 固件。SEL 是自定义文本，支持批量写、重要记录 fdatasync、尾部修复及压缩后追加；无 CRC、断电事务、多写者协调。日志默认不 fsync。队列和未落盘缓冲有界，任务不支持强制取消。芯片事实未核实处明确标注，实机电气行为需另外验证。选主/fencing、固件 A/B、完整 Redfish/RMCP+、SSE/WebSocket 属于方案未纳入的扩展方向。
