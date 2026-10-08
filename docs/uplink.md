# 上行遥测转发

本功能默认关闭。启用后订阅 sensor_state/configuration/service/recovery 四类事件，
通过独立线程向指定 IPv4 TCP 采集器发送一行一个 JSON 对象。它不接收控制命令，也不改变硬件状态。
连接建立、发送和断线检测均不阻塞采样或事件分发线程。

## 运行与部署

先在采集器 Linux 上打开一个接收端：

```sh
sudo apt-get install -y netcat-openbsd
nc -lk 9000 | tee telemetry.jsonl
```

启动 daemon；采集器与 daemon 在同一主机时使用 127.0.0.1，跨主机时替换为采集器 IP：

```sh
./build-release/bmc-lite --config config/mock.conf --rules config/rules.conf --sel var/sel.db --log var/faults.jsonl --http-port 8000 --uplink-address 127.0.0.1 --uplink-port 9000 --uplink-capacity 256
curl http://127.0.0.1:8000/healthz
curl http://127.0.0.1:8000/metrics | grep bmc_uplink
```

systemd 部署时，在现有 ExecStart 行追加以下选项，再重启服务：

```text
--uplink-address 采集器IPv4 --uplink-port 9000 --uplink-capacity 256
```

```sh
sudo systemctl daemon-reload
sudo systemctl restart bmc-lite
sudo journalctl -u bmc-lite -n 50 --no-pager
```

端口范围 1..65535，队列容量范围 1..4096，默认 256；地址与端口必须一起指定。
不支持 DNS 地址。源码和 Release 的安装过程与 README 一致，此功能不增加外部库依赖。
现有 TLS 构建开关只用于控制 HTTPS，上行当前是明文 TCP，应放在可信网络或隧道内。

## 协议与交付语义

示例：

```json
{"type": "sensor_state", "source": "cpu", "message": "critical", "sequence": 12, "time_ms": 1728000000000, "value": 95}
```

sequence 是本进程事件总线的序号，重启后重新开始；time_ms 是编码入队时的时间。
采集器需按换行组帧，不能把一次 recv 当成一条消息。字符串中的换行会被 JSON 转义。
只发送状态变化与业务事件，不持续发送所有原始采样。序号可能因事件总线或上行丢弃出现间断。

这是尽力交付的遥测，不提供 ACK、磁盘队列或断线补发保证。sent 只表示整条消息交给了
本机 socket，不表示采集器已保存。断线时丢弃未发完的帧，采集器也应丢弃 EOF 前未结束的行。
协议单向，采集器不能向此连接发送命令或响应。

等待队列满时丢弃最旧消息；另有最多一条在途消息，每条最多 8 KiB。
原始 source/message 合计超过 4 KiB，或编码后超过 8 KiB 的消息拒绝并计入丢弃。
因此单实例缓冲上限约为 (capacity + 1) × 8 KiB，加少量对象开销。
连接及发送期限均为 5 秒，重连从 250 ms 指数退避到最多 30 秒，成功发送消息后重置。
停机先停止入站与 Worker、排空事件总线，再停止上行线程；未发出的遥测计入丢弃，
不等待离线采集器，不影响 SEL 中的本地故障证据。

## 指标与验证

只在启用上行且启用只读 HTTP 时，/metrics 追加：

| 指标 | 含义 |
|---|---|
| bmc_uplink_queued | 等待队列条数，不含一条在途消息 |
| bmc_uplink_dropped_total | 溢出、超长、发送失败/超时及退出时丢弃 |
| bmc_uplink_sent_total | 整条消息交给本机 socket 的数量 |
| bmc_uplink_bytes_total | 实际发送字节，包含后来失败帧的已发部分 |
| bmc_uplink_connect_attempts_total | 连接尝试次数 |
| bmc_uplink_connected | 最近观察到的连接状态，1/0 |

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R 'Uplink|uplink_runtime' --output-on-failure
```

C++ FakeSocketIo 测试覆盖部分写/EAGAIN、5 万次入队、超时、退避、空闲断线和全部事件类型。
真实 TCP 集成测试覆盖采集器缺失、停读、连接恢复、JSONL 格式与采样持续推进。
