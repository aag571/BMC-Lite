# 运行时可靠性修复

本阶段对应 后续方案.md 的遗留 A/B/C；不增加新的监听端口或后台线程。

## 日志运行中降级

Monitor 在采样末尾和主循环刷盘后检查 Logger 的写入失败次数与丢弃字节数。
首次变化立即向 SEL 写一条重要记录，并发布 service 类型事件；持续变化最多每 5 秒报告一次。
相同计数不重复上报，时间窗口中的变化在下一次允许上报时合并到最新累计值。
告警不写回故障 Logger，避免“告警写失败又触发告警”的反馈循环。

示例 SEL 消息：

```text
source=log state=degraded message="log-write-failures=10 dropped-bytes=0"
```

实际 SEL 文件是带引号的字段格式，可查询：

```sh
sudo grep '"log" "degraded"' /var/log/bmc-lite/sel.db | tail -10
```

启用了上行遥测时，同一个 service 事件也进入上行有界队列。上行依旧是尽力交付；
本地 SEL 是这里的主要观察入口。若 SEL 同时不可写，则无法保证告警持久化。
日志持续失败不立即终止采样；退出时日志不能刷盘仍返回状态 1，并在 stderr 报告原因。

## 规则状态生命周期

每轮 Monitor.poll 结束后，FaultRuleEngine 依据当前传感器 ID 集合删除消失设备的运行时状态，
不需要等待规则热重载。仍存在设备的去抖确认计数和 active 标志保持不变。
运行时表大小因而受当前匹配的“规则 × 传感器”组合限制，不随历史设备 ID 累积。

直接使用 FaultRuleEngine 的调用方，须在一次完整设备采样后调用 retain_sensors；
不能在每个单独 evaluate 后只传当前一个设备，否则会错误清除其他设备的状态。
接口与 Monitor.poll 都在采样线程使用，不是网络线程共享对象。

## 芯片事实核验边界

docs/chips.md 逐项区分候选手册来源与核验状态。未记录确切章节/表号的事实标为“未核实”。
尤其 EMC2103 的常数、故障码、内部分辨率和默认 RANGE，及 ADM1275 命令缺失判断，
都没有在本阶段获得发布版手册确认。代码注释同步说明限制。

以前的等式 3932160 = 60 × 32768 / 5 不成立，已移除作为推导依据的表述；
不凭此猜测另一个芯片常量，也不在没有手册依据时改变现有寄存器换算。
Fake 测试证明实现与测试假设一致，不能证明真实硬件的电气行为或测量精度。

## 开发验证

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R 'RuntimeManagement|degradation_runtime' --output-on-failure
```

测试覆盖 /dev/full、运行中持久告警、5 秒报告窗口、无日志反馈、丢弃字节独立变化、
1 万次设备出现/消失、保留设备确认计数以及 Monitor 的实际裁剪接线。
源码和 Release 的安装过程不变，升级方式见 README 与控制面部署指南。
