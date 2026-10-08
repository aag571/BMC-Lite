# TCP 组件设计

本文只描述 TCP 组件的边界、协议子集、认证模型与落地顺序。它**不是**已完成功能的说明：
`bmc-lite` 当前没有任何入站网络能力，只有 `tools/bmc_manage.py` 那个独立的只读 HTTP 工具。

## 1. 现状与为什么不能只做一个端口

已经存在的东西：

| 组件 | 位置 | 能力 |
|---|---|---|
| 只读 HTTP 工具 | `tools/bmc_manage.py:97` | 监听 `127.0.0.1:8000`，`GET` 的 Redfish 形状资源 + `/metrics`，16 并发 + 32 排队 + 5 s 超时 |
| 事件总线 | `src/event_bus.cpp` | 容量 256，`publish` 满时丢弃并 `dropped()` 计数 |
| 单线程调度 | `src/main.cpp:103` | `epoll_wait` 最多 8 个事件、超时 1000 ms；`timerfd` 采样、`signalfd` 信号、可选 GPIO |
| 有界任务队列 | `src/worker.cpp` | `--task-capacity`（默认 64）、提交失败即拒绝 |
| 关闭顺序 | `src/main.cpp:170` | `worker.stop()` → `bus.stop()` → `sel.flush()` → `logger.flush()` |

需求里有四种东西：只读管理、**控制面（写操作）**、上行遥测转发、实例间通信。它们的信任模型与失败模式不同，
因此**不放进一个端口**：

| 通道 | 方向 | 信任前提 | 认证 | 失败后果 |
|---|---|---|---|---|
| 只读管理/遥测 | 入站 | 低 | 无 | 只影响可观测性 |
| **控制面** | 入站 | **高** | **强制** | 可造成物理动作 |
| 上行转发 | 出站 | 中 | 采集器侧 | 不得影响采样 |
| 实例间 | 双向 | 高（对等） | 双向 | 可导致脑裂 |

合并的代价是具体的：只读端点会继承写通道的凭证与暴露面；采集器慢会波及控制面重试；
实例间协议会变成"对等体即可改风扇"的后门；且**一个端口无法按角色分别限流与审计**。

## 2. 端口与信任模型

| 端口 | 角色 | 开关 | 默认绑定 | 默认状态 | 认证 |
|---|---|---|---|---|---|
| `127.0.0.1:8000` | 只读（对齐现有 Python 能力） | `--http-port` | 回环 | **默认关闭** | 无 |
| `0.0.0.0:8443` | 控制面 | `--control-port` + `--control-token-file` | 无 | **默认关闭（fail closed）** | 必需 |
| 出站 | 上行转发 | 配置开关 | — | 默认关闭 | 采集器侧 |

规则：

1. 控制面**未配置凭证则不创建监听套接字**，而不是"裸奔但绑回环"。fail closed。
2. 控制面一旦绑定非回环地址，**必须**启用 TLS。绑定非回环 + 无 TLS 会被视为配置错误而拒绝启动。
3. 只读端口可以不出局域网；控制端口建议只在内网管理网段可达。
4. **已定：只读服务默认关闭，必须用显式开关（`--http-port`）才监听。** 因此默认部署的攻击面与不做本组件时完全一致；
   `--help` 与文档必须把"默认关闭"写清楚，避免使用者误以为它已经开着。
5. 实例间通信**不在本设计范围内**（见第 8 节）。

## 3. 并发模型

**已定：网络服务作为 daemon 内的独立线程运行。**

两个候选与选择理由：

| 方案 | 优点 | 缺点 |
|---|---|---|
| 并入主 `epoll` | 零额外线程、天然有序 | 网络代码与采样循环共享同一线程，慢客户端/大响应会推迟采样 |
| **独立线程 + 自己的 `epoll`** | 采样节奏与网络互相隔离 | 需要把状态变更安全投递给主循环 |

选择独立线程，并遵守三条约束：

1. **网络线程只读**：读取 `SelStore::query()`（自带锁）、`EventBus` 订阅的快照、以及只读的传感器状态快照。
   不直接触碰 `MonitorSensor`、`Engine`、`FaultRuleEngine`。
2. **所有写操作经 `Worker::submit`**，由主循环/工作线程执行——与 `src/monitor.cpp:38` 的恢复提交路径同一条，
   从而复用 `RecoveryPolicyEngine` 的冷却、去重与并发上限，以及 `PwmAction` 的取值校验。
3. **禁止网络线程直接调 `write_pwm`**。这是本设计最重要的一条约束：一旦绕过，现有的冷却/去重/审计全部失效。

关闭顺序扩展为：`net.stop()`（先停入站，不再接受新请求）→ `worker.stop()` → `bus.stop()`（**最后一条出站遥测在此之前发完**）
→ `sel.flush()` → `logger.flush()`。`net.stop()` 必须在 `worker.stop()` 之前，否则在途控制请求会引用已销毁的工作线程。

## 4. socket 抽象（先做，否则会重演 `core.cpp`）

现有 `LinuxIo`（`include/bmc/linux_io.hpp`）已经证明了这个模式的价值。网络必须照做，否则会得到
"有代码但无法测试"的第二个组件。

```cpp
// include/bmc/socket_io.hpp
class SocketIo {
public:
    virtual ~SocketIo() = default;
    virtual int socket(int domain, int type, int protocol) = 0;
    virtual int bind(int descriptor, const sockaddr* address, socklen_t length) = 0;
    virtual int listen(int descriptor, int backlog) = 0;
    virtual int accept(int descriptor, sockaddr* address, socklen_t* length) = 0;
    virtual int setsockopt(int descriptor, int level, int name, const void* value, socklen_t length) = 0;
    virtual int getsockopt(int descriptor, int level, int name, void* value, socklen_t* length) = 0;
    virtual ssize_t recv(int descriptor, void* buffer, std::size_t count, int flags) = 0;
    virtual ssize_t send(int descriptor, const void* buffer, std::size_t count, int flags) = 0;
    virtual int shutdown(int descriptor, int how) = 0;
    int close(int descriptor) noexcept;  // 非虚，与 LinuxIo 一致
    int last_error = 0;
};
class PosixSocketIo final : public SocketIo { /* 直通 syscall */ };
SocketIo& system_socket_io();
```

`FakeSocketIo` 放在测试里（与 `FakeLinuxIo` 同处 `tests/core_test.cpp`），可脚本化：
accept 返回预设连接、recv 返回分片/超长/半包、send 返回部分写入或 `EAGAIN`、getsockopt 返回错误。

**用它可以单测、无需真端口的东西**：请求行/头部解析、超长头部拒绝、请求体上限、
限流与并发上限、认证失败、慢客户端不阻塞、出站队列满时丢弃并计数。

## 5. 协议子集（HTTP/1.1 的严格子集）

刻意做小而明确，避免引入框架（与项目零依赖取向一致，也不稀释"手写业务代码"口径）。

支持：
- 方法与路径：`GET`（只读端口、控制端口）、`POST`（仅控制端口）
- 头部：`Host`、`Content-Length`、`Content-Type`、`Authorization`、`Connection`
- 响应：始终带 `Content-Length`；`Cache-Control: no-store`（与现有 Python 工具一致）

明确**不支持**并在收到时返回 501/400：
- `Transfer-Encoding: chunked`
- keep-alive 复用（每条连接只处理一个请求后关闭，状态机最小化）
- 任何其他方法（`PUT`/`DELETE`/`PATCH`）、`Expect: 100-continue`、请求体分块
- HTTP/2 与 TLS 之外的升级

硬上限（超出即拒绝并计数，不做静默截断）：

| 项目 | 上限 |
|---|---|
| 请求行 | 8 KiB |
| 头部总量 | 8 KiB |
| 单个头部行 | 1 KiB |
| 请求体 | 1 MiB |
| 读超时 | 5 s |
| 写超时 | 5 s |
| 只读端口并发连接 | 64 |
| 控制端口并发连接 | 8 |
| 每个连接的 in-flight 请求 | 1 |

## 6. 端点

只读端口（与现有 Python 工具输出对齐，便于跨实现对拍）：

| 路径 | 说明 |
|---|---|
| `GET /redfish/v1/` 及其下 `Managers`、`LogServices`、`Entries` | 与 `bmc_manage.py:30` 的 `resource()` 同形状 |
| `GET /metrics` | 与 `bmc_manage.py:61` 的 `metrics()` 同文本格式 |
| `GET /healthz` | 进程存活与采样计数（本地探针用） |

控制端口：

| 路径 | 说明 |
|---|---|
| `POST /v1/actions/fan` | 提升指定传感器的风扇（走 `Worker` + `RecoveryPolicyEngine`） |
| `POST /v1/actions/inspect` | 记录一次检查请求（等价 `inspect_device`） |
| `GET /v1/config/generation` | 返回当前配置代次（只读，便于自动化确认重载生效） |

控制请求体必须包含目标**传感器 id**与动作名，**不允许**出现文件路径：
路径来自配置。允许客户端传路径会把任意文件写暴露出去，`O_NOFOLLOW` 只是最后一道防线。

## 7. 认证、审计与指标

认证（控制端口）：
- mTLS（推荐）或 `Authorization: Bearer <token>` **且必须走 TLS**
- **已定：令牌从文件读取，权限 `0640`、属主为服务账户**（`--control-token-file`），
  **不接受命令行明文传入**（会出现在 `ps` 与 journal 里）。令牌文件缺失或权限过宽时控制端口不监听。
- 校验用常数时间比较；失败返回 401 且不区分"令牌错"与"令牌缺失"之外的细节
- 失败按来源地址计数并限流（避免暴力枚举）

审计：每个控制请求写 SEL（`logger.action` + `sel.append(..., important=true)`），字段固定为
`action`、`sensor`、`outcome`（accepted/rejected/unauthorized/rate-limited）、`peer`、`request_id`。
这条链路复用现有 SEL，因此**控制操作天然进入故障证据历史**。

指标（并入 `/metrics` 文本，命名与 `bmc_sensor_*` 一致）：

```
bmc_connections_active{role="read|control"}
bmc_connections_rejected_total{role="read|control",reason="limit|timeout|parse"}
bmc_requests_total{role,method,status}
bmc_request_bytes_total{role,direction="in|out"}
bmc_control_auth_failures_total
bmc_control_rate_limited_total
bmc_uplink_queued{...} / bmc_uplink_dropped_total   # 上行转发的有界队列
```

## 8. 上行转发与实例间通信

**上行转发**（出站）：订阅 `EventBus`（`BusEventType` 现有 `sensor_state`/`configuration`/`service`/`recovery`），
写入**有界环形队列**，`connect`/`send` 全非阻塞并带指数退避重连，队列满时丢最旧并计数。
唯一的现实风险是"采集器把 daemon 拖死"，因此必须有界 + 可观测。

**实例间通信**：本设计**只规划心跳与配置代次交换**，不规划选主。
脑裂防护需要法定人数、fencing（防止旧主继续写硬件）与状态对账，在没有共享存储或外部仲裁者的前提下
自己实现，失败模式是"两个实例同时控风扇"，比不做更危险。若目标是高可用，
建议先用 systemd `Restart=on-failure`（`deploy/bmc-lite.service:11` 已有）加外部仲裁（keepalived/etcd）。

## 9. 依赖与部署

- TLS 需要 OpenSSL。做成**可选构建开关**（默认 OFF），开启时链接 OpenSSL 3.x（Apache-2.0 兼容；
  不要 1.1.1 的旧广告条款）。CMake 与 README、`deploy/install.sh` 同步。
- 证书校验**必须**校验证书链与主机名，不提供"跳过校验"开关。
- systemd 单元（`deploy/bmc-lite.service`）现有加固已包含 `NoNewPrivileges`、`ProtectSystem=strict`、
  `ProtectHome`、`PrivateTmp`、`ReadWritePaths=/var/log/bmc-lite`。控制端口只需监听能力，
  不需要新增写路径；若启用 TLS，需 `ReadOnlyPaths` 指向证书与令牌文件。
- 控制端口默认不监听，因此默认部署的攻击面与现在**完全一致**。

## 10. 分阶段落地

| 阶段 | 内容 | 可独立验证的点 | 规模 |
|---|---|---|---|
| 1 | `SocketIo` 抽象 + `FakeSocketIo` + HTTP 解析器 + 限流 | 单测，无需真端口 | 500–700 行 |
| 2 | 只读 HTTP 服务（独立线程，绑回环） | 与 Python 工具输出逐字对拍 | 600–900 行 |
| 3 | 控制面：认证 + fail-closed + 拒绝路径（先接空操作） | 无凭证不监听、401、限流、审计落 SEL | 700–1000 行 |
| 4 | 控制面接 `Worker` + `RecoveryPolicyEngine`；TLS | 冷却/去重仍生效；明文绑定非回环被拒 | 800–1200 行 |
| 5 | 上行转发（`EventBus` 订阅 + 有界队列 + 指标） | 队列满丢最旧并计数 | 400–600 行 |
| 6 | 实例间心跳（仅心跳与代次交换） | 陈旧检测 | 300–500 行 |

合计约 3.3–4.9k 行（含测试）。阶段 3 与 4 之间没有"安全的中间态"，**要么都不做，要么一起做**。

## 11. 明确不做

- 不做 keep-alive/流水线、不做 chunked、不做 HTTP/2
- 不做 WebSocket/SSE 推送（第一版只做请求-响应）
- 不做多用户/角色体系（单一控制令牌足够）
- 不做自己实现选主
- 不允许控制请求指定文件路径
- 不引入 HTTP 框架

## 12. 验收判据

1. 控制端口未配置凭证时**不创建监听套接字**（用 `ss -ltn` 验证）
2. 明文绑定非回环地址时**拒绝启动并给出明确报错**
3. 控制请求全部落入 SEL，且 `outcome` 字段可区分 accepted/rejected/unauthorized/rate-limited
4. 慢客户端（只发一半头部就不再发）在 5 s 内被断开，且不推迟采样 tick
5. 上行采集器停止接收时，队列达到上限后丢最旧并计数，采样 tick 不受影响
6. 只读端口对同一 SEL 的响应与 `tools/bmc_manage.py` 逐字一致
7. 全部新增逻辑在 `FakeSocketIo` 下可单测，且 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` 零告警
