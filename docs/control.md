# 控制面安装与使用

控制服务已接入 daemon，默认关闭。只有同时配置端口和有效令牌文件才监听。
凭证、证书、权限或绑定检查失败时，daemon 立即以非零状态退出，stderr 和日志记录原因；
运行期间控制监听或审计失效也会触发非零退出，便于 systemd 按 Restart=on-failure 重启。
只读 HTTP 与控制使用不同端口，其绑定失败仍允许监控继续采样。

## 1. 从源码构建

Ubuntu 24.04：

```sh
git clone git@github.com:aag571/BMC-Lite.git
cd BMC-Lite
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev openssl curl python3
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBMC_TLS=ON
cmake --build build-release --parallel 2
sudo bash deploy/install.sh
```

OpenSSL 需要 3.x。默认 BMC_TLS=OFF 不依赖 OpenSSL，仍可使用回环明文控制。
远程控制必须用 TLS。已有 Release 压缩包只有在打包时启用了 BMC_TLS 才能使用 HTTPS：

```sh
sha256sum -c bmc-lite-release.tar.gz.sha256
tar -xzf bmc-lite-release.tar.gz
cd bmc-lite-release
sudo apt-get install -y libssl3t64 openssl curl
sudo bash deploy/install.sh
```

安装脚本检查动态库是否缺失，并创建 /opt/bmc-lite/control 目录，但不生成或覆盖凭证。

## 2. 准备令牌和证书

```sh
sudo install -d -o bmc-lite -g bmc-lite -m 0750 /opt/bmc-lite/control
openssl rand -hex 32 > /tmp/bmc-control-token
sudo install -o bmc-lite -g bmc-lite -m 0640 /tmp/bmc-control-token /opt/bmc-lite/control/token
rm /tmp/bmc-control-token
```

令牌文件必须由实际运行进程的用户拥有，权限严格为 0640，是普通文件，不能是符号链接。
内容是至少 32 字节的可打印非空白字符，可有一个结尾换行。不能用命令行传入令牌。
本地手动运行时，将令牌文件属主设为当前用户，而不是 bmc-lite。

演示虚拟机 192.168.124.128 可生成如下自签名证书；将证书作为客户端信任锚，
私钥只保留在服务器上。正式管理网络使用内部 CA 签发的证书。

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 30 \
  -keyout /tmp/bmc-key.pem -out /tmp/bmc-cert.pem \
  -subj /CN=bmc-lite -addext 'subjectAltName=IP:192.168.124.128,DNS:localhost'
sudo install -o bmc-lite -g bmc-lite -m 0600 /tmp/bmc-key.pem /opt/bmc-lite/control/key.pem
sudo install -o root -g bmc-lite -m 0640 /tmp/bmc-cert.pem /opt/bmc-lite/control/cert.pem
rm /tmp/bmc-key.pem
```

将 cert.pem 复制到调用客户端。用 --cacert 验证证书链与 URL 主机名/IP；不使用 curl -k。
这里实现的是服务器证书加 Bearer 认证，没有实现客户端证书认证或出站 TLS。
证书与令牌在启动时读取，轮换后重启服务。

## 3. 开启 systemd 服务

```sh
sudo systemctl edit bmc-lite
```

填写以下覆盖配置；首次测试保留模拟动作，不加 --enable-actions：

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

只读服务仍绑定回环。若显式需要远程只读 HTTP，增加 --http-bind 0.0.0.0 --http-allow-remote。
控制面最多 8 个连接，读取、写入与 TLS 握手都有期限，认证失败按来源 IP 限流，
重新连接换源端口不能绕过限流。来源表满时淘汰最旧来源，避免新管理员永久被锁在表外。
非回环明文控制绑定会被拒绝。

## 4. 发起控制请求

在客户端准备权限为 0600 的 curl 配置，避免令牌出现在命令行：

```text
header = "Authorization: Bearer 替换为令牌文件内容"
cacert = "/客户端路径/cert.pem"
```

保存为 control.curl.conf，然后：

```sh
chmod 0600 control.curl.conf
curl --config control.curl.conf https://192.168.124.128:8443/v1/config/generation
curl --config control.curl.conf -H 'Content-Type: application/json' \
  --data '{"sensor":"cpu","action":"inspect_device"}' \
  https://192.168.124.128:8443/v1/actions/inspect
curl --config control.curl.conf -H 'Content-Type: application/json' \
  --data '{"sensor":"cpu","action":"increase_fan"}' \
  https://192.168.124.128:8443/v1/actions/fan
```

sensor 必须是当前配置中存在的 ID；风扇动作还需配置 action_path。请求体只接受 sensor/action
两个字符串字段，不接受路径、额外字段、重复键或字符串转义。动作路径始终来自配置快照。
202 表示已排队，不表示硬件操作已完成。重复风扇请求进入同一个恢复引擎，30 秒冷却期间不重复写硬件。
默认动作是模拟；实际 PWM 写入需启用 --enable-actions 并赋予服务账户目标节点的写权限。
真实动作只写配置指定的 PWM 节点，值为 255。

状态码：200 查询成功；202 排队；400 请求/传感器/动作不合法；401 认证失败；
429 认证失败次数超限；503 队列满或审计无法持久化。
SEL 保存 requested/accepted/rejected/unauthorized/rate-limited、action、sensor、peer、request_id。
动作先记录 requested，成功排队后才记录 accepted；审计未成功持久化时不会执行动作。
同一 request_id 后续还会记录实际恢复结果。SEL 的普通文件 fdatasync 仍可能受磁盘延迟影响。
accepted 请求验证记录与异步执行结果可通过 request_id 关联，执行结果 detail 可为 completed、cooldown 或 failed。
accepted 验证记录之后也可能出现队列拒绝或动作失败，必须查看异步结果。

## 5. 查看审计和重载

```sh
sudo grep 'control' /var/log/bmc-lite/sel.db | tail -20
sudo tail -20 /var/log/bmc-lite/faults.jsonl
sudo systemctl kill --kill-whom=main --signal=HUP bmc-lite
curl --config control.curl.conf https://192.168.124.128:8443/v1/config/generation
```

只有传感器与规则配置全部验证成功才更新代次及控制快照。失败重载保留旧配置。
已排队任务持有请求时的配置值副本，不保存传感器对象地址。

## 6. 开发验证

```sh
cmake -S . -B build-tls -DBMC_TLS=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tls --parallel 2
ctest --test-dir build-tls --output-on-failure
```

C++ 测试覆盖文件权限、符号链接、请求限制与恢复冷却；Python 集成测试启动真实 daemon，
验证 HTTP/HTTPS、证书链/主机名拒绝、PWM 临时文件、配置热加载、8 连接上限和 SEL 审计。
TLS 客户端测试使用默认验证策略；无忽略证书验证选项。
