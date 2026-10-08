# BMC-Lite 总体设计说明

**一句话定位**：本文回答「这套代码怎么组织、类与类之间约定了什么、配置文件怎么写、
进程启动后按什么顺序做事」，面向要改代码、调配置或排查「它为什么不动作」的人。

**与架构文档的分工**：

| 文档 | 回答的问题 |
|---|---|
| [BMC-Lite架构设计文档](BMC-Lite架构设计文档.md) | 为什么这样分层、放弃了哪些方案、代价是什么 |
| **本文** | 文件放在哪、接口契约是什么、配置怎么写、生命周期怎么走 |

第一次接触项目建议先读架构文档的第一节；只想动手加一个后端或改一条规则，
可以直接从本文第三节开始。凡是标「未核实」的地方，表示当前代码里找不到确证。

---

## 目录

- [一、开发原因 为什么还需要一份总体设计说明](#一开发原因-为什么还需要一份总体设计说明)
- [二、设计思路 这份说明按什么原则组织](#二设计思路-这份说明按什么原则组织)
- [三、系统构成 具体长什么样](#三系统构成-具体长什么样)
- [四、优缺点与适配性](#四优缺点与适配性)
- [附：延伸阅读](#附延伸阅读)

---

## 一、开发原因 为什么还需要一份总体设计说明

**本节要讲什么**：项目已经有 README 和架构文档，为什么还需要第三份文档。

### 1.1 三份文档各管一段

| 文档 | 回答什么 | 不回答什么 |
|---|---|---|
| [README](../README.md) | 怎么装、怎么跑、怎么看日志 | 代码怎么组织 |
| [架构设计文档](BMC-Lite架构设计文档.md) | 为什么这么分层、代价是什么 | 某个函数失败时返回什么 |
| **本文** | 文件放哪、接口怎么约定、配置怎么写、进程怎么走 | 设计取舍的论证 |

### 1.2 没有它会怎样

这不是「文档洁癖」，缺了这一层会直接产生四类具体问题。

| 缺少的信息 | 直接后果 |
|---|---|
| `Reader` 与 `Device` 分工 | 容易把设备生命周期塞进 `Reader`，或把业务判断塞进 `Device` |
| 错误语义约定 | 同一层出现两种风格：有人返回 `nullopt`，有人抛异常，调用方无法统一处理 |
| 配置字段与顺序 | 改配置只能试错：11 个必填字段的顺序、`-` 的含义都只能从代码里反推 |
| 启动与退出顺序 | 踩坑：先停 `Worker` 再停网络，会让网络线程的提交全部变成 rejected |

本文的目标就是把这四件事一次写清楚，让「照着写」就能得到正确结果。

---

## 二、设计思路 这份说明按什么原则组织

**本节要讲什么**：代码组织遵循哪几条原则，以及为此放弃过哪些做法。

### 2.1 四条组织原则

1. **目录按依赖方向分，不按文件类型分。** `include/` 提供契约，`src/` 提供实现，
   `tests/` 提供证据，`tools/` 提供操作入口，`config/` 提供数据，`deploy/` 提供落地方式。
2. **一个概念只有一个权威定义。** `Config` 只在 `core.hpp` 定义一次；规则语义只在
   `rules.cpp` 实现一次。看到第二份定义，基本可以判定是重复或过期代码。
3. **接口窄、实现宽。** `Reader` 只有一个 `read()`，把生命周期交给 `Device`；
   这样测试只需要伪造一个方法，而不是一整套设备行为。
4. **配置是数据，不是代码。** 阈值、方向、窗口、标定、动作路径全部来自文本文件，
   代码里不出现机型常量。

### 2.2 考虑过但放弃的做法

| 做法 | 放弃原因 |
|---|---|
| 把 `core.hpp` 拆成十几个小文件 | 会引入大量互相包含与可见性管理，收益只是「单文件更短」 |
| 每个设备后端一个 `.cpp` | 后端共享大量路径解析与错误分支，拆开要么重复，要么再抽一层公共头 |
| 引入依赖注入框架 | 只有一条生产装配路径，手写构造函数注入已经够用，框架会带来构建依赖 |
| 配置支持条件、包含、环境变量 | 解析器复杂度上升，且校验期与运行期会出现两套求值结果 |
| 让动作可插件注册 | 动作是白名单枚举（`increase_fan`、`inspect_device`），开放注册会扩大攻击面 |

这些取舍与架构文档的结论一致，只是角度不同：那边讲「为什么这样分层」，
这里讲「因此文件该放哪、写成什么样」。

---

## 三、系统构成 具体长什么样

**本节要讲什么**：这一节是可查手册——目录怎么分、构建出哪几个目标、
每个接口的契约是什么、配置字段怎么填、进程怎么启动与退出。

### 3.1 目录与构建目标

#### 3.1.1 目录分工

| 目录 | 放什么 | 谁读它 |
|---|---|---|
| `include/bmc/` | 15 个公共头文件（契约） | 调用方、测试 |
| `src/` | 27 个实现文件 | 维护者 |
| `tests/` | 16 个测试文件 | 提交前验证 |
| `tools/` | 6 个运维与代码生成脚本 | 部署、发布 |
| `config/` | 3 个示例配置 | 首次运行 |
| `deploy/` | systemd 单元、安装与自检脚本 | 上线 |
| `docs/` | 中文文档（部分带英文对照） | 阅读 |

#### 3.1.2 `include/bmc/` 的 15 个头文件

| 头文件 | 职责 |
|---|---|
| `core.hpp` | 核心类型层：设备与状态机、配置与标定、事件总线、规则、SEL 与日志、恢复策略、Worker |
| `chip.hpp` | 芯片层：`I2cBus` 的 SMBus 访问、`ChipDriver` 抽象、驱动注册表与寄存器解码辅助 |
| `linux_io.hpp` | Linux 系统调用的可注入抽象与生产实现，供 I2C/GPIO/PWM 复用 |
| `socket_io.hpp` | socket 系统调用的可注入抽象、生产实现与地址格式化 |
| `monitor.hpp` | 采样循环：传感器准备、每 tick 的读值与分发 |
| `action.hpp` | 恢复动作接口与两个实现（`LogOnlyAction`、`PwmAction`） |
| `cli.hpp` | 命令行选项结构与解析入口 |
| `http.hpp` | HTTP/1.1 严格子集的解析与限流（含令牌桶） |
| `json.hpp` | JSON 写出器，输出与 `tools/bmc_manage.py` 的 `json.dumps` 对齐 |
| `service.hpp` | 只读服务面：SEL 读取、响应渲染、资源与指标文本 |
| `network.hpp` | 网络层：单连接 HTTP 状态机、epoll 服务器、TLS 接线与指标 |
| `control.hpp` | 控制面：令牌校验、常数时间比较、动作端点的鉴权与审计 |
| `tls.hpp` | TLS 服务端/客户端上下文与每连接的 `SocketIo` 装饰器 |
| `uplink.hpp` | 上行遥测：订阅总线、有界队列、非阻塞重连与发送指标 |
| `peer.hpp` | 对等通道：主动心跳、失效判定、代次交换与心跳端点服务 |

#### 3.1.3 `src/` 的 27 个实现文件

按架构文档的四层归类（层与层之间只允许上层依赖下层）：

- **domain（纯判断）**：`engine.cpp`、`rules.cpp`、`calibration.cpp`、`state.cpp`
- **hardware（真的读写设备）**：`readers.cpp`、`chips.cpp`、`linux_io.cpp`、`socket_io.cpp`
- **infrastructure（通用设施）**：`fd.cpp`、`logger.cpp`、`sel.cpp`、`event_bus.cpp`、
  `worker.cpp`、`recovery.cpp`、`actions.cpp`
- **application（装配与生命周期）**：`main.cpp`、`monitor.cpp`、`config.cpp`、`cli.cpp`
- **网络服务**：`http.cpp`、`json.cpp`、`service.cpp`、`network.cpp`、`control.cpp`、
  `tls.cpp`、`uplink.cpp`、`peer.cpp`

> `main.cpp` 是唯一没有对应头文件的源文件：它定义 `main()` 与 `run()`，
> 是进程唯一负责生命周期的地方。

> 上面按**主要职责**归类。`linux_io.cpp`、`socket_io.cpp`、`state.cpp`、`actions.cpp`
> 正好处在层次边界上，把它们放到相邻一层也能说通，不必当成硬性规定。

#### 3.1.4 `tests/`、`tools/`、`config/`、`deploy/` 各自放什么

| 位置 | 内容 |
|---|---|
| `tests/` | 7 个 C++ 用例、8 个 Python 进程用例、1 个对拍向量头文件 |
| `tools/` | 6 个文件：验证、打包、写入基准、SEL 工具、向量生成 |
| `config/` | `mock.conf`（模拟）、`hardware.conf.example`（实机示例）、`rules.conf`（规则） |
| `deploy/` | systemd 单元、安装与自检脚本，以及 `qemu/` 集成环境 |

`tools/` 里的 6 个文件各管一件事：

- `validate.sh`（本地验证入口）、`package-release.sh`（生成 Release 发行包）。
- `bench-writes.sh` + `bench_writes.cpp`（写入路径基准）。
- `bmc_manage.py`（SEL 运维工具，同时是只读响应层的参考实现）。
- `gen_service_vectors.py`（把参考实现的输出固化成对拍向量）。

#### 3.1.5 CMake 的 5 个构建目标

| 目标 | 类型 | 内容 |
|---|---|---|
| `bmc_core` | 静态库 | 26 个实现文件（`src/` 去掉 `main.cpp`），对外暴露 `include/` |
| `bmc-lite` | 可执行文件 | `src/main.cpp`，安装到 `bin/` |
| `bench-writes` | 可执行文件 | `tools/bench_writes.cpp`，性能基准，不参与安装 |
| `bmc_tests` | 可执行文件 | 6 个 C++ 测试文件 + GoogleTest，用 `gtest_discover_tests` 注册 |
| `bmc_stress` | 可执行文件 | `tests/stress_test.cpp`，作为 `scheduler_stress` 测试运行 |

构建相关的三个开关与一条依赖约定：

| 项 | 默认 | 说明 |
|---|---|---|
| `BMC_SANITIZE` | OFF | 打开 ASan + UBSan |
| `BMC_TSAN` | OFF | 打开 ThreadSanitizer；与 `BMC_SANITIZE` 互斥，会直接报错 |
| `BMC_TLS` | OFF | 需要 OpenSSL 3，启用控制面与心跳的 HTTPS |
| GoogleTest | 1.15.2 | 由 `FetchContent` 固定版本下载，不计入项目代码行数 |

`bmc_core`、`bmc-lite`、`bench-writes` 三个目标统一使用
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`；
安装规则把 `bmc-lite` 装到 `bin/`，配置装到 `share/bmc-lite/config`，
文档装到 `share/doc/bmc-lite`。

### 3.2 接口契约

#### 3.2.1 `Config` 的 12 个字段

`Config` 是「一路传感器」的完整定义，只在 `include/bmc/core.hpp` 定义一次。

| 字段 | 默认值 | 含义 |
|---|---|---|
| `id` | 无（必填） | 传感器唯一标识；规则、控制面路径表与指标标签都按它索引 |
| `backend` | 无（必填） | 取值来源：`mock`/`sysfs`/`i2c`/`gpio`/`i2c:<chip>[@<feature>]` |
| `path` | 无（必填） | 取值位置，语法由 `backend` 决定 |
| `scale` | 1 | 读数的比例系数；必须有限且非 0 |
| `high` | true | 阈值方向：`true` 表示越大越危险 |
| `warning` | 70 | 警告阈值 |
| `critical` | 90 | 危险阈值 |
| `hysteresis` | 3 | 迟滞带宽，只影响恢复，必须小于两档阈值间距 |
| `debounce` | 3 | 连续确认次数，0 被拒绝 |
| `failure_limit` | 3 | 连续读取失败上限，0 被拒绝 |
| `action_path` | 空 | 恢复动作目标（PWM 文件）；配置文件里写 `-` 表示没有 |
| `calibration` | gain=1、offset=0 | 可选第 12 字段：线性修正 + 分段线性插点 |

#### 3.2.2 `Reader` 与 `Device` 的关系

一句话：**`Reader` 只管「读一个数」，`Device` 管「一路设备的生命周期」。**

```text
Config ──make_reader()──▶ Reader（只有一个 read()）
   │                          ▲
   └──make_device()──▶ AdapterDevice ──内部持有一个 Reader
                          │
                          ├─ info()        静态身份与能力（来自配置）
                          ├─ state()       运行状态：closed/ready/degraded/failed
                          ├─ open()/close()  构造或释放 Reader
                          ├─ probe()       打开后试读一次
                          ├─ read_value()  读值；失败返回 nullopt 并降级为 degraded
                          └─ write_value() 只对可写设备有效，0..255 的整数
```

两者的契约差异：

| 问题 | `Reader` | `Device` |
|---|---|---|
| 关心什么 | 一次取值 | 一路设备的整个生命周期 |
| 读失败怎么表示 | 返回 `std::nullopt` | 返回 `std::nullopt`，并把 `state` 降为 degraded |
| 构造失败怎么表示 | 抛 `std::invalid_argument` | 被 `AdapterDevice::open()` 吞掉并记为 failed |
| 是否知道业务 | 不知道 | 不知道 |

#### 3.2.3 `Engine::update` 的返回语义

| 情况 | 返回值 |
|---|---|
| 状态没有真正变化 | `std::nullopt`（调用方不需要再去重） |
| 状态发生迁移 | 一个 `Event`，含旧状态、新状态、当前值、原因与序号 |
| 读数为空（读取失败） | 未达 `failure_limit` 时可能仍是 `nullopt`；达到后迁移到 `unavailable` |

构造 `Engine` 时会调用 `validate()`，因此非法配置在装载阶段就抛
`std::invalid_argument`，不会等到第一次采样。

#### 3.2.4 `FaultRuleEngine` 三个方法的分工

这三个方法很容易混，分工如下：

- `evaluate(event)`：评估所有规则、累积确认计数，返回本轮真正产生的激活/清除决策。
  每轮采样、每个传感器都要调用。
- `merge(rules, sensors)`：热重载时替换规则集，并按「规则 id + 触发状态 + 传感器仍在」
  保留可沿用的状态；只在 `SIGHUP` 重载成功后调用。
- `retain_sensors(sensors)`：用当前传感器集合裁剪运行时状态表，
  丢弃已消失传感器的计数与激活标志；每轮采样结束时调用。

配套的两个约定：

- 构造函数与 `validate()` 用同一套判据，所以重载前可以先整体校验，保证重载原子。
- 运行时状态的键是「规则 id + 触发状态 + 传感器 id」，所以改了 `trigger` 的规则
  一定不会继承旧状态的计数。

#### 3.2.5 其余基础服务的职责

这些服务遵循同一个约定：**正常路径返回状态，故障路径降级并计数，
不把异常抛回采样循环。** 逐项如下：

- `EventLogger` / `Logger`：事件流水与动作审计。`Logger` 是文件实现，每行一条 JSON，
  按大小轮转。写入失败只计数不抛出；只有轮转后重开文件失败才抛 `std::system_error`。
- `SelStore`：故障证据档案；常驻描述符追加。
  当 `important=true` 时立即 `write+fdatasync`；写入失败只计数。
  启动时会修复尾部的残缺记录。
- `EventBus`：进程内单分发线程的事件总线，按类型过滤。
  `publish` 队列满时丢弃并计数，不阻塞发布方；处理器异常被吞掉。
- `Worker`：有界优先级线程池；任务按优先级降序出队。
  `submit` 在队列满、已停止或任务为空时返回 `false` 并计入 rejected。
- `RecoveryPolicyEngine`：负责去重、冷却（默认 30 秒）、并发上限与有限重试。
  被拒绝时仍返回结果（`accepted=false`）；只有状态表满且无法回收才返回 `nullopt`。

#### 3.2.6 错误语义总表

初学者最容易踩的坑是「什么时候会抛、什么时候只是返回值变空」。
这张表就是全文的判据。

| 情况 | 行为 |
|---|---|
| 配置行非法、阈值不自洽、重复 id | 抛 `std::invalid_argument`，带行号，进程不启动 |
| 配置文件打不开 | 抛 `std::runtime_error` |
| 某次设备读取失败 | 返回 `nullopt`，累计失败次数，**不抛异常、不退出** |
| PWM 写入失败 | `write_value` 返回 `false`，设备状态降为 degraded |
| 日志或档案写入失败 | 只计数并上报降级，**不退出** |
| 控制面端口与令牌不成对 | 抛异常，进程以非零码退出（fail-closed） |
| 只读 HTTP 启动失败 | 记日志、写 stderr，进程继续跑，最终退出码为 1 |

### 3.3 配置与规则语法

#### 3.3.1 传感器文件：11 个必填字段 + 1 个可选字段

每行一个传感器，空白分隔，字段顺序固定：

```text
id backend path scale direction warning critical hysteresis debounce failure_limit action_path
```

| 序号 | 字段 | 说明 |
|---:|---|---|
| 1 | `id` | 传感器唯一标识，同文件内不得重复 |
| 2 | `backend` | `mock`/`sysfs`/`i2c`/`gpio`/`i2c:<chip>[@<feature>]` |
| 3 | `path` | 取值位置，语法见 3.3.2 |
| 4 | `scale` | 比例系数，必须有限且非 0 |
| 5 | `direction` | `high`（越大越危险）或 `low`（越小越危险） |
| 6 | `warning` | 警告阈值 |
| 7 | `critical` | 危险阈值 |
| 8 | `hysteresis` | 迟滞带宽，必须严格小于两档阈值间距 |
| 9 | `debounce` | 连续确认次数，1~1000000 |
| 10 | `failure_limit` | 失败上限，1~1000000 |
| 11 | `action_path` | PWM 目标路径；`-` 表示没有动作 |
| 12 | `calibration` | **可选**，语法见 3.3.3 |

解析规则中有几个刻意的严格之处：

- 第 12 个字段之后**不允许**再有字段：多出来就整行非法，避免字段错位被静默忽略。
- `debounce` 与 `failure_limit` 只接受纯数字，`3abc` 这类半截数字会被拒绝。
- 整行为空或以 `#` 开头（可带前导空白）才是注释，行内 `#` 属于字段内容。
- 文件里一个传感器都没有视为错误：没有可做的事就不该启动。

#### 3.3.2 五种后端与 `path` 语法

| `backend` | 实现 | `path` 语法 |
|---|---|---|
| `mock` | `MockReader` | 逗号分隔的读数序列，如 `45,75,err`；`err` 表示该次失败，序列循环播放 |
| `sysfs` | `SysfsReader` | 单个取值文件的绝对路径；只接受恰好一个数值 token |
| `i2c` | `RawI2cReader` | `device,address,register`；用 SMBus word read 事务 |
| `gpio` | `GpioReader` | `chip,offset[,active-low]`，如 `/dev/gpiochip0,17` |
| `i2c:<chip>` | `ChipReader` | `device,address`；语义归驱动负责，`<feature>` 选测量量 |

两条容易踩的边界：

- `i2c` 后端在**构造时**就检查适配器是否支持 `I2C_FUNC_SMBUS_READ_WORD_DATA`，
  不支持直接拒绝，而不是让每次采样静默失败。
- 芯片型号在装载阶段就与 `chip_names()` 比对，拼错型号不会拖到构造期才暴露；
  `<feature>` 由驱动校验。

芯片后端的完整写法是 `i2c:<chip>[@<feature>]`，例如 `i2c:lm75@temp`：
`@` 后面是测量量；省略时取该型号声明的第一个测量量。

#### 3.3.3 标定字段语法

第 12 个字段的语法是：

```text
gain[:offset][;raw=value;raw=value...]
```

| 形式 | 效果 |
|---|---|
| 省略或写 `-` | 不做任何标定（gain=1、offset=0、无标定点） |
| `2` | 只做线性缩放 |
| `2:1.5` | 先乘 gain 再加 offset（`corrected = raw * gain + offset`） |
| `1;10=0;100=50` | 线性修正后，按标定点做分段线性插值 |
| `2:0.5;0=0;50=100` | 两者同时使用 |

标定的校验约束：gain 必须为正；标定点的 `raw` 必须严格递增；所有数值必须有限；
超出首末标定点的输入会被夹到端点值。

#### 3.3.4 规则文件格式

每行固定 6 个字段：

```text
id sensor state confirmations clear_confirmations action
```

| 字段 | 取值 | 说明 |
|---|---|---|
| `id` | 非空且唯一 | 规则标识，决策与日志按它索引 |
| `sensor` | 传感器 id 或 `*` | `*` 表示作用于任意传感器，各传感器独立计数 |
| `state` | `warning`/`critical`/`unavailable` | 触发状态 |
| `confirmations` | 1~1000000 | 连续命中多少次才激活 |
| `clear_confirmations` | 1~1000000 | 连续不命中多少次才清除 |
| `action` | `increase_fan`/`inspect_device` | 内置白名单，不能自定义 |

项目自带的 `config/rules.conf` 就是三个例子：

```text
# id sensor state confirmations clear_confirmations action
cpu-critical cpu_temp critical 1 2 increase_fan
fan-failure fan_rpm critical 1 2 increase_fan
any-unavailable * unavailable 1 1 inspect_device
```

有一个容易误解的细节：**触发 `warning` 的规则在 `critical` 时同样算命中**
（严重度升级），所以升级不会先清除再重新激活；反过来，触发 `critical` 的规则
遇到 `warning` 不算命中，会按 `clear_confirmations` 正常清除。

#### 3.3.5 热加载时哪些状态会保留

| 情况 | 保留状态？ |
|---|---|
| 规则 id 和触发状态都没变 | ✅ 保留确认计数与激活标志 |
| 触发状态被改写 | ❌ 丢弃（旧的 active 对应不同判定条件） |
| 规则被删除 | ❌ 丢弃 |
| 传感器消失 | ❌ 丢弃 |
| `confirmations` 或 `action` 改变 | ✅ 保留（判定身份没有变） |

### 3.4 调度与生命周期

#### 3.4.1 启动顺序

顺序本身是契约的一部分，尤其是「屏蔽信号必须在建线程之前」。

1. 忽略 `SIGPIPE`：OpenSSL 内部写 socket 不使用 `MSG_NOSIGNAL`，
   断开的客户端不能杀死整个守护进程。
2. 用 `pthread_sigmask` 屏蔽 `SIGINT`、`SIGTERM`、`SIGHUP`——
   **必须在创建任何线程之前**，此后所有线程都继承该掩码，信号只会出现在 `signalfd` 上。
3. 构造 `Logger`、`SelStore`（声明在最前，因而最后析构）。
4. 若配置了上行地址，构造并启动 `Uplink`。
5. 构造 `EventBus`，注册全部订阅（订阅必须早于任何事件发布，
   否则 `started` 这类启动事件不会上行）。
6. 构造 `PosixLinuxIo`，`prepare_sensors()` 装载并构造全部传感器。
7. 装载规则；规则为空不算错误，但会记一条 warning。
8. 选择动作实现：`PwmAction` 或默认的 `LogOnlyAction`；构造恢复策略引擎。
9. 构造 `Monitor` 与 `Worker`。
10. 配置控制面：端口与令牌文件必须成对出现，任何失败都让进程起不来。
11. 配置实例心跳：四个参数必须齐全。
12. 创建只读 HTTP 服务；启动失败不是致命错误，只体现在最终退出码上。
13. 创建 `epoll`、`timerfd`、`signalfd`，可选 `--gpio` 的 `EPOLLPRI` 登记项。
14. 记录并发布 `started`，随后进入主循环。

#### 3.4.2 主循环每个 tick 做什么

主循环是一个 `epoll_wait` 事件循环，三个登记项各有明确分工：

| 就绪项 | 处理 |
|---|---|
| `signalfd` | `SIGHUP` → 原子热重载；`SIGINT`/`SIGTERM` → 置停止标志 |
| `timerfd` | 走一轮采样；读出的值就是错过的 tick 数，大于 1 会记一条 `missed timer ticks` |
| GPIO（可选） | 立即补一次采样，不等下一个 tick |

每轮循环开始前先检查三件事：控制监听线程是否失败、控制面审计是否失败、
心跳监听线程是否失败；任一失败就抛异常，让 systemd 的 `Restart=on-failure` 生效。
`epoll_wait` 的 1 秒超时只是兜底唤醒。

一次 `Monitor::poll` 内部的固定顺序：

1. 逐个传感器读值 → `Engine::update` → 迁移时写日志、写 SEL（关键）、发总线事件。
2. 无论是否迁移都构造一个「当前状态」事件交给 `FaultRuleEngine::evaluate`，
   这样 `confirmations` 大于 1 的规则才能累积到确认。
3. 每条决策写审计日志与 SEL，发总线事件；激活则 `Worker::submit`（优先级 10），
   清除则 `recovery.reset()` 释放冷却状态。
4. 收尾顺序固定：SEL 降级上报 → `retain_sensors` 裁剪 → 日志降级上报。

`--ticks N` 用于有限步运行：完成第 N 个 tick 后即使没有信号也会退出。

#### 3.4.3 `SIGHUP` 热重载：先全量校验，再替换

重载必须原子。步骤固定为：

1. 重新构造传感器集合与规则集合（任一构造失败即进入失败分支）。
2. 对新的规则集调用 `FaultRuleEngine::validate()` 整体校验。
3. 全部通过后，先发布 `validated generation N+1` 事件——外部看到该代次即可认为生效。
4. `sensors.swap(replacement)`：新配置从下一个 tick 起生效。
5. `rules.merge()`：合并而不是整体替换，避免重复触发一次恢复动作。
6. 代次加一，刷新控制面的路径快照，把代次同步给心跳对端。
7. 任何一步抛异常都在 `catch` 里记 `reload rejected; keeping generation N`，
   旧配置与旧规则原样保留。

`--check-config` 走的是同一套校验路径，因此「离线校验通过」与
「重载能被接受」的判据一致。

#### 3.4.4 退出顺序

退出顺序不能随意调换，原因是**还有别的线程在往队列里提交任务**。

| 顺序 | 动作 | 为什么是这个位置 |
|---:|---|---|
| 1 | 停止只读 HTTP | 它不再产生新的读请求 |
| 2 | 停止心跳监听与心跳线程 | 对端通道先收口 |
| 3 | 停止控制面监听 | 最后关闭能触发动作的入口 |
| 4 | `worker.stop()` | 排空已接受的任务；必须在网络之后 |
| 5 | 发布 `stopping` 事件 | 让订阅者看到收尾 |
| 6 | `bus.stop()` | 总线排空后，异步写入才全部结束 |
| 7 | 停止 `Uplink` 并记录发送/丢弃统计 | 出站通道最后收口 |
| 8 | 统计 Worker 结果并写日志/SEL | 此时数据才不再变化 |
| 9 | `sel.flush()` 与 `logger.flush()` | 常驻描述符 + 批量写，退出前必须落盘 |

如果把第 4 步提到第 1 步之前，网络线程的提交会直接变成 rejected。

#### 3.4.5 退出码与命令行默认值

| 退出码 | 含义 |
|---:|---|
| 0 | 正常停止，且日志刷盘成功、只读监听启动成功且运行中未失败 |
| 1 | 运行期异常（含配置错误、控制面失败、控制面审计失败），或上述条件之一不满足 |
| 2 | 命令行解析失败（同时打印用法） |

启动时的关键默认值：

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--config` | `config/mock.conf` | 传感器配置 |
| `--rules` | `config/rules.conf` | 规则文件 |
| `--log` | `var/faults.jsonl` | 事件流水 |
| `--sel` | `var/sel.db` | 故障档案 |
| `--interval-ms` | 1000 | 采样周期 |
| `--worker-threads` | 2 | 恢复线程数 |
| `--task-capacity` | 64 | 任务队列上限 |
| `--enable-actions` | 关闭 | 关闭时只记日志，不写 PWM |
| `--http-port` | 0 | 0 表示不监听只读接口 |
| `--ticks` | 0 | 0 表示一直运行 |

---

## 四、优缺点与适配性

**本节要讲什么**：这套组织方式的优点、它付出的代价，以及为什么它适配这个项目。

### 4.1 优点

| 优点 | 具体体现 |
|---|---|
| 可测 | 系统调用集中在 `LinuxIo`/`SocketIo`，测试用一个假实现就能覆盖寄存器解码与失败路径 |
| 无框架依赖 | 除可选 OpenSSL 与测试用的 GoogleTest 外，只用 C++20 标准库与 pthread |
| 构建目标少 | 5 个目标，职责一眼可辨；没有代码生成，没有自定义构建步骤 |
| 契约集中 | 12 个字段的 `Config`、6 个方法的 `Device` 都在 `core.hpp` 一处定义 |
| 配置与代码分离 | 阈值、方向、窗口、标定、动作路径全在文本里，可 diff、可评审 |
| 失败语义统一 | 读失败一律 `nullopt`，配置错误一律抛异常，调用方不需要猜 |

### 4.2 代价

| 代价 | 具体表现 |
|---|---|
| `core.hpp` 偏大 | 408 行、20 多个类型集中在一个文件，定位类型要翻文件 |
| 实现文件偏集中 | 所有后端挤在 `readers.cpp`（381 行） |
| 装配集中 | `main.cpp` 的 `run()` 约 320 行，一条直线读完整个进程的生命周期 |
| CMake 手写源文件列表 | 新增 `.cpp` 必须同时改 `CMakeLists.txt`，忘记改会出现链接错误 |
| 进程测试依赖环境 | Python 用例需要真实二进制与可写目录，不能只跑单测 |
| 类型数量多 | 同一份 `core.hpp` 服务四层，读者需要先建立整体印象再逐层深入 |

### 4.3 为什么这样组织适配这个项目

| 项目约束 | 组织方式 | 如果不这样会怎样 |
|---|---|---|
| 小团队维护 | 接口集中在少数文件、装配写在一条直线里 | 分散到 40 个文件后，改动要先找一圈 |
| 需要长期可读 | 一个概念只有一个权威定义 | 定义漂移后，改对一处仍然出错 |
| 要在无硬件环境验证 | `include/` 只放抽象，实现可替换 | 无法在没有 I2C/GPIO 的机器上验证 |
| 现场不方便升级依赖 | 不引入框架与中间件 | 部署多一步就可能装不上 |
| 配置由运维改 | 纯文本、行式语法、装载期全量校验 | 错误配置会带进运行期 |

### 4.4 边界与未做的事

| 未做 | 说明 |
|---|---|
| 后端插件注册 | 后端类型是 `make_reader()` 里的固定分派，新增后端要改代码 |
| 配置包含与条件 | 只支持「一个传感器一行」的平铺格式 |
| 每个后端独立文件 | `readers.cpp` 集中所有 Reader 与 `AdapterDevice` |
| 依赖注入框架 | 装配由 `main.cpp` 手写 |
| 动态加载 | 没有 `.so` 插件机制；`bmc_core` 与 `bmc-lite` 一起编译 |
| `core.hpp` 的拆分计划 | 当前是明确保留的取舍，不是待办事项 |

---

## 附：延伸阅读

| 想了解 | 看哪里 |
|---|---|
| 为什么要这样分层、放弃了什么 | [BMC-Lite架构设计文档](BMC-Lite架构设计文档.md) |
| 逐文件职责与阅读顺序 | [代码阅读指南](代码阅读指南.md) |
| 芯片寄存器、标定与后端细节 | [芯片驱动与标定](芯片驱动与标定.md) |
| 验证方式与覆盖范围 | [验证与测试报告](验证与测试报告.md) |
| 日志与档案的写入性能 | [写入路径性能基准](写入路径性能基准.md) |
| 网络端口与信任模型 | [网络设计说明](网络设计说明.md) |
| 运行期降级行为 | [运行时可靠性说明](运行时可靠性说明.md) |
| 控制面部署与使用 | [控制面部署与使用](控制面部署与使用.md) |
| 上行遥测部署与使用 | [上行遥测部署与使用](上行遥测部署与使用.md) |
| 实例心跳部署与使用 | [实例心跳部署与使用](实例心跳部署与使用.md) |
| 从零安装、编译、部署 | [README](../README.md) |
