# 项目验收记录 / Acceptance record

验收范围是已收敛的 Linux C++20 简历项目，以及后续方案与 TCP 方案的任务 1–6、遗留 A–D。
报告中的完整 Redfish/RMCP+、DSL、选主、固件升级等扩展方向没有纳入当前方案；不以它们代替已承诺功能，也不声称完整 BMC 固件。

The accepted scope is the Linux C++20 resume project and tasks 1–6 plus residual issues A–D in the follow-up/TCP plans. Full Redfish/RMCP+, DSL, elections and firmware updates are outside this scope.

## 功能与证据 / Implementation evidence

| 要求 | 实现 | 验证 |
|---|---|---|
| 单一实现、Linux I/O 注入 | 分离源文件、LinuxIo/SocketIo、C++ Fake | core_test.cpp、network/control/uplink/peer_test.cpp |
| GPIO character-device | GPIO v2 请求、线值、失败、CLOEXEC、RAII | FakeIo.GpioLineValuesFailureAndDescriptorLifetime |
| 采样/规则/恢复闭环 | Monitor → rules → Worker → Recovery → Action → SEL/Bus | C++ 领域、编排及 Linux 进程测试 |
| 有界线程调度 | 有界队列、优先级、并发、排空、统计 | scheduler_stress、TSan、Valgrind |
| SEL 加固 | 常驻 fd、批量写、fdatasync、尾部修复、压缩后切换 fd、短写后缀重试 | Sel.*，重复压缩后从磁盘逐次读取 |
| 任务 1/2：只读网络 | 独立 epoll、64 连接、绝对期限、增量读和部分写、默认关闭 | HttpConnection.*、network_runtime、Python 响应层向量 |
| 任务 3/4：控制 | 独立 8 连接、文件令牌、认证/限流、重要审计、Worker/Recovery、可选 TLS | Control.*、control_runtime、control_tls_runtime |
| 网络指标 | 活跃/拒绝/请求/字节、认证失败/限流、固定角色与有限标签 | network_runtime 的解析/超时/65 连接验证 |
| 任务 5：上行 | EventBus → 有界丢最旧 → 非阻塞重连、指标 | Uplink.*、uplink_runtime |
| 任务 6：心跳 | 独立端口与令牌、双向代次、陈旧/恢复、CA/主机名校验 | PeerHeartbeat.*、peer_runtime、peer_tls_runtime |
| 遗留 A：日志降级 | SEL/Bus 上报计数变化、5 秒节流、不递归写失败日志 | RuntimeManagement.*、degradation_runtime |
| 遗留 B：规则状态 | 按当前传感器裁剪，热加载保留有效确认状态 | 10000 个消失传感器测试、runtime_reload |
| 恢复状态内存 | 最多 4096 状态，到期回收；不淘汰运行中/冷却中的保护状态 | Recovery.DisappearingSensorsHaveBoundedCooldownState / StateCapacityPreservesExistingCooldowns |
| 遗留 C：芯片依据 | 无章节证据的事实明确标为未核实 | chips.md 和源码注释，不推测硬件参数 |
| 遗留 D：Linux 验证 | Ubuntu 24.04 / GCC 13，真实构建和执行 | 最终测试结果与 benchmarks.md |
| 安装/使用/Release | 中英文从 clone 到安装、systemd、热加载、网络、查询与升级；TLS 压缩包和 SHA256 | README、README.en、package-release.sh、解压后运行验证 |

## 验证方式 / Validation

最终代码在开发 Linux 实测：

| 检查 | 结果 |
|---|---|
| 默认 TLS OFF 构建与 CTest | 133/133 通过 |
| TLS ON + ASan/UBSan（含真实 HTTPS/双实例证书失败测试） | 135/135 通过，无报告 |
| TSan（setarch -R） | 133/133 通过，无数据竞争报告 |
| Valgrind 全部 C++ 用例 | 123/123 通过，0 errors，退出时 0 bytes / 0 blocks |
| Valgrind 并发压力程序 | 0 errors，退出时 0 bytes / 0 blocks |
| 普通压力测试重复 20 次 | 全部通过 |
| 写入基准 50000 条 | 六场景完成，实际 syscall 数和磁盘/tmpfs 对照见 benchmarks.md |
| 编译告警 / 补丁空白 | 严格告警参数下无告警，git diff --check 通过 |
| TLS Release 压缩包 | 从包内解压出的二进制通过默认关闭/HTTP、控制 HTTPS、双实例心跳 TLS 集成测试；动态库无缺失，未链接 Sanitizer |

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build-tls --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-sanitize --output-on-failure
BMC_TSAN_NO_ASLR=1 bash tools/validate.sh tsan
bash tools/validate.sh valgrind
bash tools/validate.sh stress
bash tools/bench-writes.sh 50000
```

GCC TSan 在本机默认地址映射下无法初始化；使用 setarch -R 构建和运行后正常执行测试。不是忽略报告，也没有把失败当成功。该选项仅作用于测试进程，部署不需要关闭 ASLR。

## 使用边界 / Deployment limits

目标虚拟机 192.168.124.128 的 root/systemd 安装由用户按 README 执行，本次交付验证的是开发 Linux 与可解压运行的 Release。真实 I2C/GPIO 电气、PWM 安全值和厂商寄存器必须在目标设备验证。心跳无选主/fencing；上行无 ACK/重放；SEL 无 CRC/断电事务；任务不支持强制取消。这些边界不以测试通过替代。
