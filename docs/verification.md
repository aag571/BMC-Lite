# 验证证据 / Verification evidence

本文汇总各子系统的实现要点与对应的验证方式，以及当前版本实测通过的检查项。
设计依据见 [architecture.md](architecture.md)、[framework.md](framework.md) 与 [tcp.md](tcp.md)，
使用与部署见 [../README.md](../README.md)。

未纳入范围的方向：完整 Redfish/RMCP+、DSL、选主/fencing、固件升级。
BMC-Lite 不是完整 BMC 固件，也不以这些方向代替已实现的功能。

## 功能与验证证据 / Implementation evidence

| 能力 | 实现要点 | 验证 |
|---|---|---|
| 单一实现、Linux I/O 注入 | 分离源文件、LinuxIo/SocketIo、C++ Fake | core_test.cpp、network/control/uplink/peer_test.cpp |
| GPIO character-device | GPIO v2 请求、线值、失败、CLOEXEC、RAII | FakeIo.GpioLineValuesFailureAndDescriptorLifetime |
| 采样/规则/恢复闭环 | Monitor → rules → Worker → Recovery → Action → SEL/Bus | C++ 领域、编排及 Linux 进程测试 |
| 有界线程调度 | 有界队列、优先级、并发、排空、统计 | scheduler_stress、TSan、Valgrind |
| SEL 加固 | 常驻 fd、批量写、fdatasync、尾部修复、压缩后切换 fd、短写后缀重试 | Sel.*，重复压缩后从磁盘逐次读取 |
| 只读管理面 | 独立 epoll、64 连接、绝对期限、增量读和部分写、默认关闭 | HttpConnection.*、network_runtime、Python 响应层向量 |
| 控制面 | 独立 8 连接、文件令牌、认证/限流、重要审计、Worker/Recovery、可选 TLS | Control.*、control_runtime、control_tls_runtime |
| 网络指标 | 活跃/拒绝/请求/字节、认证失败/限流、固定角色与有限标签 | network_runtime 的解析/超时/65 连接验证 |
| 上行遥测 | EventBus → 有界丢最旧 → 非阻塞重连、指标 | Uplink.*、uplink_runtime |
| 实例心跳 | 独立端口与令牌、双向代次、陈旧/恢复、CA/主机名校验 | PeerHeartbeat.*、peer_runtime、peer_tls_runtime |
| 日志运行中降级 | SEL/Bus 上报计数变化、5 秒节流、不递归写失败日志 | RuntimeManagement.*、degradation_runtime |
| 规则状态生命周期 | 按当前传感器裁剪，热加载保留有效确认状态 | 10000 个消失传感器测试、runtime_reload |
| 恢复状态内存 | 最多 4096 状态，到期回收；不淘汰运行中/冷却中的保护状态 | Recovery.DisappearingSensorsHaveBoundedCooldownState / StateCapacityPreservesExistingCooldowns |
| 芯片寄存器依据 | 无章节证据的事实明确标为未核实 | chips.md 和源码注释，不推测硬件参数 |
| 安装与发布 | 从 clone 到安装、systemd、热加载、网络、查询与升级；TLS 压缩包和 SHA256 | README、README.en.md、package-release.sh、解压后运行验证 |

## 测试覆盖 / Test coverage

C++ 用例是主体，Python 脚本启动真实进程验证编排层：

| 目标 | 内容 |
|---|---|
| `bmc_tests` | 领域状态机、配置与规则、标定、芯片寄存器解码、注入式 I/O、SEL/日志加固、HTTP 解析与限流、只读响应层与 Python 参考实现的逐字对拍 |
| `bmc_stress` | 并发生产者下的任务队列与事件总线计数守恒 |
| Python 运行时用例 | 只读 HTTP、控制面（含 HTTPS）、双实例心跳、上行遥测、SIGHUP 热加载、故障注入、日志降级 |

## 实测结果 / Measured results

开发与验证环境：Ubuntu 24.04 x86_64、GCC 13，Release 与 Debug 分别构建。

| 检查 | 结果 |
|---|---|
| 默认构建（`BMC_TLS=OFF`）与 CTest | 132/132 通过 |
| `BMC_TLS=ON` + ASan/UBSan（含真实 HTTPS 与双实例证书校验失败用例） | 134/134 通过，无报告 |
| TSan（`setarch -R`） | 132/132 通过，无数据竞争报告 |
| Valgrind（全部 C++ 用例） | 123/123 通过，0 errors，退出时 0 bytes / 0 blocks |
| Valgrind（并发压力程序） | 0 errors，退出时 0 bytes / 0 blocks |
| 普通压力测试重复 20 次 | 全部通过 |
| 写入基准 50000 条 | 六场景完成，实际 syscall 数与磁盘/tmpfs 对照见 benchmarks.md |
| 编译告警 | `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` 下无告警 |
| TLS Release 压缩包 | 包内二进制通过默认关闭/HTTP、控制 HTTPS、双实例心跳 TLS 集成测试；动态库无缺失，未链接 Sanitizer |

复现命令：

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build-tls --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-sanitize --output-on-failure
BMC_TSAN_NO_ASLR=1 bash tools/validate.sh tsan
bash tools/validate.sh valgrind
bash tools/validate.sh stress
bash tools/bench-writes.sh 50000
```

GCC TSan 在默认地址映射下无法初始化，使用 `setarch -R` 构建和运行后正常执行测试。
该选项仅作用于测试进程，部署不需要关闭 ASLR。

## 已知边界 / Known limits

真实 I2C/GPIO 电气行为、PWM 安全值和厂商寄存器必须在目标设备上验证；虚拟机测试不能替代。
心跳无选主/fencing；上行无 ACK 与断线重放；SEL 无 CRC 与断电事务保证；已提交任务不支持强制取消。
这些边界不以测试通过替代。
