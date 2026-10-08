# BMC-Lite

Linux C++20 硬件状态监测与故障自愈原型。架构和框架设计见 docs。当前实现包含 epoll/timerfd/signalfd、模拟/sysfs/SMBus word 读取、带去抖和迟滞的状态机、有界恢复线程、PWM 控制和滚动 JSONL 日志。

## 构建和测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/bmc-lite --config config/mock.conf --interval-ms 20 --ticks 40
```

测试需要首次下载固定版本 GoogleTest，生产构建可用 -DBUILD_TESTING=OFF。没有 root 权限也能运行模拟。实机读取需要 sysfs/i2c-dev 权限；PWM 写入需要 --enable-actions 和对应节点的写权限。默认只记录 dry-run。

## 配置

每行字段为 id backend path scale direction warning critical hysteresis debounce failure_limit action_path [calibration]。示例见 config。mock 序列循环播放。阈值等值算异常；迟滞边界等值保持故障状态。读取失败打断连续异常去抖，连续失败确认 unavailable，重新读取成功后也需要确认窗口。

I2C 有两种后端：`i2c` 是原始无符号 SMBus word，比例由配置决定；`i2c:<chip>[@<feature>]` 走专用芯片驱动，支持 lm75 / lm75b / emc2103 / adm1275 / ina219 / ina226，负责符号扩展、字节序、位域与 LSB 换算。可选的标定字段形如 `gain[:offset][;raw=value;...]`，先线性修正再做分段线性插值（区间外钳制）。寄存器依据与已知限制见 docs/chips.md。GPIO 参数使用已经导出并配置 edge 的旧 sysfs value 节点，现代 GPIO character-device API 尚未实现。

## 运行

```sh
./build/bmc-lite --config config/mock.conf --log var/faults.jsonl
./build/bmc-lite --config config/hardware.conf --gpio /sys/class/gpio/gpio23/value
```

SIGTERM/SIGINT 有序停止并 drain 恢复队列。Critical 状态变化可提交一次恢复，冷却 30 秒。当前不对持续 Critical 自动重试。日志写入失败仍会让服务退出（故障证据不可丢）；SEL 写入失败改为可观测降级：计数、上报事件，并在 1 MiB 上限内保留未落盘数据，不会让 daemon 消失。没有网络管理面、关机/复位脚本。硬件专用驱动只覆盖 docs/chips.md 列出的六种型号，其余芯片仍需 `i2c` 原始寄存器后端加标定。

## 完成度

这是第一版可运行核心，尚未达到用户要求的 10000 行手写业务代码。业务规模只统计 include 和 src 的非空非注释行，测试、文档、依赖和生成代码不计入。不得将本原型描述为已完成全部项目需求。
