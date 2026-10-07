# BMC-Lite 架构设计

## 目标与边界

以 Linux 用户态 C++20 服务实现温度、电压、风扇转速采集、故障判断、结构化日志和受控恢复。无真实 BMC 硬件时，模拟输入必须覆盖同一条处理链。管理接口先采用本地命令行与日志，不要求远程 TCP。真实设备的路径、比例、阈值和恢复目标由配置定义。

## 数据流

配置 -> HardwareMonitor -> Sample -> ThresholdEngine -> FaultEvent -> FaultLogger / RecoveryExecutor

epoll 统一调度 timerfd 和 signalfd。hwmon 通常不支持可靠的 POLLPRI，因此默认定时读取；GPIO sysfs 中断可以独立注册 EPOLLPRI。事件循环只负责采样和状态转换；恢复动作交给有界工作队列，不能阻塞事件循环。

## 分层

1. domain：采样、阈值策略、告警状态、去抖与迟滞。
2. hardware：模拟、sysfs 和 Linux i2c-dev 读取，GPIO 事件节点。
3. infrastructure：RAII 文件描述符、epoll、定时器、信号、滚动 JSON 日志、有界线程池。
4. application：配置校验、监控编排、恢复去重和冷却、命令行。

## 故障状态机

Normal / Warning / Critical / Unavailable。高温和低转速采用可配置方向。连续异常计数达到窗口后升级；迟滞区间保持当前等级；恢复需要连续正常样本。读取失败单独累计，达到门限进入 Unavailable。NaN 与无穷值视为读取失败，不能参与数值比较。状态变化才生成 FaultEvent，事件包含旧状态、现值、原因、时间和序号。

## 恢复与权限

默认 dry-run。只允许明确配置的 PWM 文件写入，不执行任意 shell。告警恢复有冷却窗口，同一传感器不重复提交，队列满时记录拒绝事件。真实写入需要 --enable-actions，运行账户必须对目标文件有权限。不在虚拟机触发主机重启、关机或任意 reset。

## 可观测性与可靠性

JSONL 日志支持大小轮转和保留数量。采样错误不能退出整个服务。配置错误阻止启动。SIGINT/SIGTERM 经 signalfd 触发有序停止，工作线程 drain 后退出。所有设备 fd 使用 RAII；I2C 失败必须保留 errno。日志失败作为服务异常退出，避免失去故障证据。

## 验证与限制

GoogleTest 验证边界、去抖、迟滞、读取失败与恢复；参数矩阵使用独立预期计算。模拟集成验证真实 epoll 循环、日志和信号退出。ASan/UBSan 用于内存与未定义行为检查。虚拟机测试不能证明真实 I2C/GPIO/PWM 电气行为，实机验收需要目标板及权限。
