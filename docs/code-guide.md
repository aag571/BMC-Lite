# 代码阅读指南

建议阅读顺序：core.hpp 的领域模型 → engine.cpp → config.cpp → readers.cpp → monitor.hpp/monitor.cpp → main.cpp → worker.cpp/recovery.cpp。

## 文件职责

- engine.cpp：纯数值判断、迟滞、连续样本确认，不访问文件和设备。
- config.cpp：传感器配置解析与验证。
- rules.cpp：规则文件解析、每传感器规则状态与激活/清除。
- readers.cpp：mock/sysfs/I2C/GPIO 读取、设备适配与注册。
- actions.cpp 与 action.hpp：PWM 系统调用和可替换动作策略。
- monitor.cpp：采样、状态持久化、规则判断、异步恢复提交。
- main.cpp：参数解析、应用依赖构造、Linux epoll/timerfd/signalfd 调度和重载。
- worker.cpp：有界任务队列和线程生命周期。
- event_bus.cpp：异步通知、类型过滤和背压。
- logger.cpp、sel.cpp：滚动日志与事件存储。
- fd.cpp：独占文件描述符所有权和移动语义。

## 为什么有采样快照与状态事件两种数据

规则确认需要每个采样周期的状态，否则连续 critical 不产生新事件，confirmations 大于 1 的规则就无法触发。Monitor 每次都评估快照，但只将状态迁移写入 SEL，避免每周期生成相同故障记录。

## 异步生命周期

恢复请求复制设备路径及 ID，不保存 Sensor 地址。配置替换可以销毁旧 Sensor，但不会破坏已提交请求。Worker 必须先排空，总线随后排空，日志和 SEL 最后销毁。旧的共享 pending 原子标记已删除，去重和冷却集中在恢复策略引擎。

## 可替换接口

Action 用于选择 LogOnlyAction 或 PwmAction；EventLogger 是监控逻辑的日志接口，Logger 为文件实现。测试可自定义记录型日志实现。Reader/Device 隔离采集业务，但底层 open/ioctl 尚未注入可模拟系统调用接口，因此 I2C/GPIO 实机路径仍不能全部用单元测试替代。

## 当前拆分边界

保留 core.hpp 作为兼容的公共声明入口，避免一次引入大量互相包含的头文件。Reader 的具体实现仍集中 readers.cpp；没有为每个设备创建单独文件。Monitor 为无状态业务协调器，依赖由调用者提供；应用运行时负责持有传感器和基础服务。没有引入依赖注入框架。
