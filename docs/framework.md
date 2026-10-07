# BMC-Lite 框架设计

## 目录与构建目标

include/bmc/core.hpp 定义领域契约与基础服务；src 下按职责拆分配置、状态机、设备、日志、线程与服务编排；src/main.cpp 编排 Linux 事件循环；tests 覆盖领域与硬件读写；tools/generate_matrix.py 产生确定性 GoogleTest 边界矩阵；config 包含模拟和实机示例。

CMake 提供 bmc_core 静态库、bmc-lite 程序和 bmc_tests 测试目标，使用 target-based CMake、Threads 与 GoogleTest。测试依赖采用固定版本 FetchContent，不把依赖行数计入项目。

## 接口

SensorConfig 定义 id、backend、path、scale、direction、warning、critical、hysteresis、debounce、failure_limit 和 action_path。Reader 根据 backend 返回可选数值。Engine::update 返回可选 Event，仅在确认状态变化时返回事件。Logger::write 输出 JSONL 并滚动。Worker::submit 返回是否接受任务。Monitor 对各传感器拥有独立 Reader 和 Engine。

## 配置语法

每行一个传感器，空白分隔：id backend path scale direction warning critical hysteresis debounce failure_limit action_path。backend 为 mock/sysfs/i2c。mock path 为逗号分隔读数，err 代表读取失败；i2c path 为 device,address,register，采用 SMBus word read；direction 为 high/low；action_path 为 - 时无动作。解析时拒绝重复 id、未知字段、非有限数、无效窗口及阈值方向。

## 调度与生命周期

main 先屏蔽终止信号，再创建线程，保证信号由 signalfd 接收。timerfd 每个 tick 遍历传感器。GPIO 参数可选，事件发生后立即额外采样。--ticks 支持有限步模拟测试。每次事件追加日志，Critical 可提交 PWM 恢复；成功提交时记录冷却时间，失败不消耗冷却窗口。

## 代码量口径

用户要求至少 10000 行手写业务代码。排除空白、注释、测试、自动生成文件、第三方库、构建产物、文档和配置。通过工具统计 include 和 src 的有效行数，未达到目标时必须如实说明，不能以测试或格式填充替代业务实现。
