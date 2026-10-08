# BMC-Lite 框架设计

## 目录与构建目标

`include/bmc/` 是公共接口层：`core.hpp` 汇总领域契约与基础服务，其余头文件按子系统划分
（`chip` / `cli` / `control` / `http` / `json` / `linux_io` / `monitor` / `network` / `peer` /
`service` / `socket_io` / `tls` / `uplink`）。`src/` 按职责拆分配置、状态机、设备、日志、
线程与服务编排；`tests/` 覆盖领域与硬件读写，并由 Python 脚本补充真实进程的编排测试；
`tools/` 提供 SEL 管理工具、响应层向量生成器与验证/打包脚本；`config/` 包含模拟与实机示例；
`deploy/` 包含 systemd 单元、安装器与 QEMU 集成环境。

CMake 提供 `bmc_core` 静态库、`bmc-lite` 程序、`bench-writes` 基准程序和 `bmc_tests` /
`bmc_stress` 测试目标，使用 target-based CMake、Threads 与 GoogleTest。
测试依赖采用固定版本的 FetchContent，不把依赖行数计入项目。

## 接口

`Config` 定义 `id`、`backend`、`path`、`scale`、`high`、`warning`、`critical`、`hysteresis`、
`debounce`、`failure_limit`、`action_path` 和可选的 `Calibration`。
`Reader` 按 backend 返回可选数值，`Device` 在其上提供生命周期与读写适配。
`Engine::update` 返回可选 `Event`，仅在确认状态变化时返回事件。
`FaultRuleEngine::evaluate` 返回本轮的 `RuleDecision` 列表；`merge` 与 `retain_sensors`
分别负责热重载时的状态保留与每轮采样后的裁剪。
`Logger::write` 输出 JSONL 并滚动；`SelStore::append` 以 `important` 区分是否立即落盘。
`Worker::submit` 返回是否接受任务；`RecoveryPolicyEngine::submit` 负责去重、冷却与重试。
所有系统调用经 `LinuxIo` / `SocketIo` 注入，因此上述逻辑都能在没有真实设备与端口的情况下测试。

## 配置语法

每行一个传感器，空白分隔：`id backend path scale direction warning critical hysteresis debounce failure_limit action_path`，
以及可选的 `calibration`（第 12 个字段，语法 `gain[:offset][;raw=value;raw=value...]`）。
backend 为 `mock`、`sysfs`、`i2c`、`gpio` 或 `i2c:<chip>[@<feature>]`。
mock 的 path 是逗号分隔读数，`err` 代表读取失败；原始 i2c 的 path 是 `device,address,register`，
采用 SMBus word read；gpio 的 path 是 `chip,offset[,active-low]`；专用芯片的 path 是 `device,address`。
direction 为 `high`/`low`；action_path 为 `-` 时无动作。
解析时拒绝重复 id、未知后端、非有限数、无效窗口、阈值方向错误、非正 gain 与非严格递增的标定点。

规则文件每行：`id sensor state confirmations clear_confirmations action`。
state 为 `warning`/`critical`/`unavailable`，sensor 可用 `*` 通配，
action 为 `increase_fan` 或 `inspect_device`。

## 调度与生命周期

main 先屏蔽终止信号、再创建线程，保证信号由 signalfd 接收。timerfd 每个 tick 遍历传感器；
`--gpio` 可选，事件发生后立即额外采样；`--ticks` 支持有限步运行。
每次状态迁移追加日志、写入 SEL 并发布事件；规则确认后提交 Worker 执行恢复，
成功提交时记录冷却时间，失败不消耗冷却窗口。
只读、控制与心跳监听是各自独立的线程与 epoll，采样节奏与网络互不阻塞。
退出时先停止网络，再排空 Worker，随后停止事件总线，最后落盘日志与 SEL。
