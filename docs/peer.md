# 实例心跳部署与使用

心跳是独立通道，不共享控制令牌，不连接 Worker/Recovery/设备。默认关闭，只交换配置代次与存活信息。每个实例同时监听和定期请求对端，支持陈旧、恢复和 SIGHUP 后的新代次。不实现选主、fencing 或故障切换。

## 本机两实例演示

在仓库根目录生成令牌；两个进程必须使用相同令牌。令牌必须属于运行账户，权限严格 0640，普通文件且不是符号链接。

```sh
mkdir -p var/peer-demo
openssl rand -hex 32 > var/peer-demo/token
chmod 0640 var/peer-demo/token
# 终端 A：
./build/bmc-lite --config config/mock.conf --rules config/rules.conf \
  --log var/peer-demo/a.log --sel var/peer-demo/a.sel --http-port 8001 \
  --peer-address 127.0.0.1 --peer-port 9002 --peer-listen-port 9001 \
  --peer-token-file var/peer-demo/token --peer-interval-ms 1000 --peer-stale-ms 5000
# 终端 B：
./build/bmc-lite --config config/mock.conf --rules config/rules.conf \
  --log var/peer-demo/b.log --sel var/peer-demo/b.sel --http-port 8002 \
  --peer-address 127.0.0.1 --peer-port 9001 --peer-listen-port 9002 \
  --peer-token-file var/peer-demo/token --peer-interval-ms 1000 --peer-stale-ms 5000
# 终端 C：
curl -s http://127.0.0.1:8001/metrics | grep bmc_peer
```

终止 B，超过 5 秒后 A 的 `bmc_peer_stale` 为 1，SEL/日志/事件总线记录 `peer stale`；重新启动 B 后记录 `peer recovered`。对 B 发 SIGHUP 后，A 的 `bmc_peer_generation` 更新。首次从启动起计时，没有任何有效心跳也会超时。重复失联不刷屏。

## 跨虚拟机 TLS 部署

先按 README 安装两个实例，按 control.md 的证书步骤分别为各自 IP 生成带 SAN 的证书。心跳使用自己的 token 文件，例如 `/opt/bmc-lite/control/peer-token`，同一内容以服务账户所有、0640 分别安装。不要把它用作控制令牌。

将 A 的公开证书复制到 B 作为 `peer-ca.pem`，B 的公开证书复制到 A；私钥留在各自服务器。内部 CA 环境使用 CA 文件替代自签证书。所有文件放在 systemd 可读的 `/opt/bmc-lite/control`，私钥 0600；token 0640。两端均用 TLS 构建。

在 A 的 systemd ExecStart 追加（对端 B 为 192.168.124.128）：

```text
--peer-bind 0.0.0.0 --peer-listen-port 9443 --peer-address 192.168.124.128 --peer-port 9443 --peer-token-file /opt/bmc-lite/control/peer-token --peer-ca /opt/bmc-lite/control/peer-ca.pem --peer-server-name 192.168.124.128 --peer-tls-cert /opt/bmc-lite/control/cert.pem --peer-tls-key /opt/bmc-lite/control/key.pem
```

B 反向填写 A 的实际 IP，`peer-server-name` 必须对应 A 证书 SAN（DNS 名也可以）。保留完整的 config/rules/sel/log 参数；需要指标时保留 `--http-port 8000`。override 添加 `ReadOnlyPaths=/opt/bmc-lite/control`，执行 daemon-reload/restart，管理网防火墙允许双方的 9443。证书、令牌轮换要重启。

非回环出站必须提供 CA/服务器名，非回环监听必须提供证书/私钥；错误 CA、主机名或令牌不能更新对端状态，没有跳过验证开关。启动失败撤销心跳监听，采样继续，最终退出 1。只有回环可省略 TLS。

## 协议、限制与指标

`GET /v1/heartbeat`，头部 `Authorization: Bearer ...` 和 `X-BMC-Generation: 正整数`。响应 200 的短文本是接收方当前代次加换行。其他路径/写操作返回 400，错误凭证 401/429。一个连接处理一次交换，最多 8 个入站连接，响应缓冲最多 8 KiB。令牌 32..512 字符；建议 64 位十六进制随机令牌。

出站为非阻塞 connect/send/recv，完整交换期限为 min(5 秒, stale_ms)，重试间隔由 interval_ms 给定（至少 10ms，stale 必须大于 interval）。每实例只有一个在途交换，不积压任务；stop 最多等待线程的一个 10ms 周期。有效入站或经过验证的出站响应都会刷新存活时间。

指标：`bmc_peer_stale`、`bmc_peer_generation`、`bmc_peer_heartbeats_total`、`bmc_peer_failures_total`，以及 role="peer" 的网络指标。对端代次只是观察值，不会自动复制配置。真实网络抖动可造成陈旧告警，需要合理设置超时。
