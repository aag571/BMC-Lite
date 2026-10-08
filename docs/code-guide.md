# 代码阅读指南

建议阅读顺序：core.hpp 的领域模型 → engine.cpp → config.cpp → calibration.cpp → readers.cpp → chips.cpp → monitor.hpp/monitor.cpp → main.cpp → worker.cpp/recovery.cpp。

## 文件职责

- engine.cpp：纯数值判断、迟滞、连续样本确认，不访问文件和设备。
- calibration.cpp：gain/offset 线性修正与分段线性插值（区间外钳制）。
- config.cpp：传感器配置解析与验证（含可选的标定字段与 `i2c:<chip>` 后端）。
- rules.cpp：规则文件解析、策略校验、每 (规则, 传感器) 运行时状态与激活/清除，以及 SIGHUP 重载时的状态合并（`merge`）。
- readers.cpp：mock/sysfs/原始 I2C/GPIO 读取、设备适配与注册。
- chips.cpp：I2C 总线封装（I2cBus）与专用芯片驱动（LM75、EMC2103、ADM1275、INA219/INA226）。
  寄存器语义与换算依据见 docs/chips.md。
- actions.cpp 与 action.hpp：PWM 系统调用和可替换动作策略。
- monitor.cpp：采样、状态持久化、规则判断、异步恢复提交。
- main.cpp：参数解析、应用依赖构造、Linux epoll/timerfd/signalfd 调度和重载。
- worker.cpp：有界任务队列和线程生命周期。
- event_bus.cpp：异步通知、类型过滤和背压。
- logger.cpp、sel.cpp：滚动日志与事件存储。两者都用常驻描述符与批量写入（`LogPolicy` / `Truncate`）；
  日志默认不 fsync 且轮转前先落盘当前分片，SEL 对关键证据立即 `write+fdatasync` 并在启动时修复尾部残缺记录。
  量化结果见 docs/benchmarks.md（`tools/bench-writes.sh`）。
- fd.cpp：独占文件描述符所有权和移动语义。
- linux_io.cpp：系统调用注入层（PosixLinuxIo + 进程级默认实例）。

## 为什么有采样快照与状态事件两种数据

规则确认需要每个采样周期的状态，否则连续 critical 不产生新事件，confirmations 大于 1 的规则就无法触发。Monitor 每次都评估快照，但只将状态迁移写入 SEL，避免每周期生成相同故障记录。

## 异步生命周期

恢复请求复制设备路径及 ID，不保存 Sensor 地址。配置替换可以销毁旧 Sensor，但不会破坏已提交请求。Worker 必须先排空，总线随后排空，日志和 SEL 最后销毁。旧的共享 pending 原子标记已删除，去重和冷却集中在恢复策略引擎。

## 可替换接口

Action 用于选择 LogOnlyAction 或 PwmAction；EventLogger 是监控逻辑的日志接口，Logger 为文件实现。测试可自定义记录型日志实现。Reader/Device 隔离采集业务；底层 open/ioctl/read/write/close 已经收敛到 `LinuxIo`（`include/bmc/linux_io.hpp`），由应用层注入 `PosixLinuxIo`，测试注入 `tests/core_test.cpp` 里的 `FakeLinuxIo`。因此 I2C 的地址/寄存器校验、SMBus word 解码、能力位拒绝、以及 PWM 写入失败路径都有单元测试覆盖。

GPIO 仍是部分覆盖：`GPIO_V2_GET_LINE_IOCTL` 由内核分配 line 描述符，假实现无法提供，因此 GPIO 只覆盖"请求参数构造"（offset 越界、active-low 标志、后续 fcntl 失败），line fd 的生命周期与实机电气行为仍需目标板或 QEMU 验证。系统调用接口之外的部分（epoll/timerfd/signalfd 调度）仍无单元测试，只能靠 `tests/runtime_test.py` 端到端覆盖。

## 当前拆分边界

保留 core.hpp 作为兼容的公共声明入口，避免一次引入大量互相包含的头文件。Reader 的具体实现仍集中 readers.cpp；没有为每个设备创建单独文件。Monitor 为无状态业务协调器，依赖由调用者提供；应用运行时负责持有传感器和基础服务。没有引入依赖注入框架。
