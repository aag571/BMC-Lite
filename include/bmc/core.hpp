#pragma once
// 核心类型层：设备与状态机、配置与标定、事件总线、故障规则、SEL 与日志、恢复策略、Worker。
// 这些声明的实现分散在 src/config.cpp、engine.cpp、event_bus.cpp、rules.cpp、sel.cpp、
// logger.cpp、recovery.cpp、worker.cpp、state.cpp、fd.cpp 与 actions.cpp。
#include "bmc/linux_io.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>

namespace bmc {
// 设备生命周期状态：closed 只在显式关闭或构造之后出现，unavailable 表示读取持续失败。
enum class DeviceState { closed, ready, degraded, failed };
// 设备的静态身份与能力，来自配置而非探测结果；运行状态与测量值不在这里。
struct DeviceInfo {
    std::string id;
    std::string kind;
    std::string path;
    std::string description;
    bool readable = false;
    bool writable = false;
};
// 一台设备的统一接口：探测与单次读写。读失败用 std::nullopt 表示，写失败用 false 表示。
class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceInfo& info() const = 0;
    virtual DeviceState state() const = 0;
    virtual bool open() = 0;
    virtual void close() noexcept = 0;
    virtual bool probe() = 0;
    virtual std::optional<double> read_value() = 0;
    virtual bool write_value(double value) = 0;
};
// 设备容器：拥有所有 Device，按 id 线性查找，并支持整体 open/close。
class DeviceRegistry {
public:
    void add(std::unique_ptr<Device> device);
    // 未命中返回 nullptr。
    Device* find(const std::string& id) const;
    std::vector<DeviceInfo> inventory() const;
    // 只要有一台设备打开失败就返回 false，但已打开的其余设备保持打开。
    bool open_all();
    void close_all() noexcept;
private:
    std::vector<std::unique_ptr<Device>> devices_;
};
// 判级后的告警级别。name() 给出稳定字符串，写入日志、SEL 与指标标签。
enum class State { normal, warning, critical, unavailable };
std::string name(State state);
// JSON 字符串转义：引号、反斜杠与 C0 控制字符，非 ASCII 原样保留。
std::string escape(const std::string& value);
// 描述符的 RAII 包装。析构走真实 ::close，因此不可拷贝，只能移动。
class Fd {
public:
    explicit Fd(int value = -1);
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    // 未持有描述符时返回 -1。
    int get() const;
private:
    int value_;
};
// 多点标定：raw 经 gain/offset 线性修正后，再按校准点做分段线性插值。
// 空的 points 表示纯线性转换，行为与只使用 scale 时一致。
struct Calibration {
    double gain = 1;
    double offset = 0;
    std::vector<std::pair<double, double>> points;
};
// 把原始读数转换为工程值：先线性修正，再按 points 分段线性插值；
// 超出首末校准点的输入被夹到端点值。输入非有限时抛出 std::invalid_argument。
double apply(const Calibration& calibration, double raw);
// 一路传感器的完整定义（含判级方向、去抖窗口、故障上限与标定）。
struct Config {
    std::string id;
    std::string backend;
    std::string path;
    double scale = 1;
    // true 表示"越大越坏"（warning < critical）；配置文件里写作 high/low，含义是阈值方向。
    bool high = true;
    double warning = 70;
    double critical = 90;
    double hysteresis = 3;
    unsigned debounce = 3;
    unsigned failure_limit = 3;
    std::string action_path;
    Calibration calibration;
};
// 逐行解析配置文件并校验，返回全部传感器。文件打不开抛出 std::runtime_error，
// 非法行或空配置抛出 std::invalid_argument。
std::vector<Config> load_config(const std::filesystem::path& path);
// 校验单个传感器定义；任何违反约束之处抛出 std::invalid_argument。
void validate(const Config& config);
// 一次状态迁移的完整记录，写入日志与 SEL；value 为空表示迁移由读取失败触发。
struct Event {
    std::string id;
    State before;
    State after;
    std::optional<double> value;
    std::string reason;
    std::uint64_t sequence;
};
// 总线事件分类；订阅按类型过滤，不做通配。
enum class BusEventType { sensor_state, configuration, service, recovery };
// 事件总线上的载荷：source 标识来源子系统，sequence 由总线在入队时统一编号。
struct BusEvent {
    BusEventType type;
    std::string source;
    std::string message;
    std::optional<double> value;
    std::uint64_t sequence = 0;
};
// 单分发线程的进程内事件总线：publish 只入队，按类型过滤后在工作线程调用处理器。
// 队列满时丢弃本次事件并计数（不会阻塞发布方），处理器异常被吞掉以免拖垮分发线程。
class EventBus {
public:
    using Handler = std::function<void(const BusEvent&)>;
    // 容量为 0 或线程创建失败会抛出异常。
    explicit EventBus(std::size_t capacity = 256);
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    // 返回订阅 id 供诊断；重复订阅同一类型会各自收到回调，处理器为空或总线已停止会抛出异常。
    std::uint64_t subscribe(BusEventType type, Handler handler);
    // 入队成功返回 true；队列已满或总线已停止返回 false，两种情况都不阻塞。
    bool publish(BusEvent event);
    // 幂等；返回前 join 分发线程，此后 publish/subscribe 都不再接受新事件。
    void stop();
    // 因队列满而丢弃的累计事件数。
    std::uint64_t dropped() const;
private:
    struct Subscription { std::uint64_t id; BusEventType type; Handler handler; };
    void dispatch();
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<BusEvent> queue_;
    std::vector<Subscription> subscriptions_;
    std::uint64_t next_id_ = 1;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t dropped_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};
// 一条故障规则的声明式定义：sensor 进入 trigger 后，需连续 confirmations 次确认才生效，
// 恢复同样需要 clear_confirmations 次，生效时执行 action。
struct FaultRule {
    std::string id;
    std::string sensor;
    State trigger = State::critical;
    unsigned confirmations = 1;
    unsigned clear_confirmations = 1;
    std::string action;
};
// 规则对一个事件给出的判定结果：active 为真表示该规则当前处于触发态，附带原因 action 与序号。
struct RuleDecision {
    std::string rule;
    std::string sensor;
    State state;
    bool active;
    std::string action;
    std::uint64_t sequence;
};
// 规则状态机：把状态事件折算成带确认计数的触发/恢复决定，并支持不丢状态的规则热重载。
class FaultRuleEngine {
public:
    explicit FaultRuleEngine(std::vector<FaultRule> rules);
    std::vector<RuleDecision> evaluate(const Event& event);
    void reset();
    // 校验一组规则是否符合策略；热重载在改动任何状态之前调用它，保证重载是原子的。
    static void validate(const std::vector<FaultRule>& rules);
    // 热重载：按"规则 id 与触发状态都没变"保留该规则在每个传感器上的确认计数与 active 标志。
    // 已消失的传感器、被删除的规则、以及触发状态被改写的规则，其运行时状态一律丢弃，
    // 否则保留下来的 active 会对应到不同的判定条件上。
    void merge(std::vector<FaultRule> rules, const std::vector<std::string>& sensors);
    // 每轮采样用当前传感器集合清理消失设备的状态；不清除仍在运行的确认计数。
    void retain_sensors(const std::vector<std::string>& sensors);
    std::size_t runtime_size() const { return runtime_.size(); }
private:
    struct Runtime { unsigned bad = 0; unsigned good = 0; bool active = false; std::uint64_t sequence = 0; };
    std::vector<FaultRule> rules_;
    std::unordered_map<std::string, Runtime> runtime_;
};
// 解析规则文件。文件打不开抛出 std::runtime_error，非法或重复规则抛出 std::invalid_argument。
// 纯文本解析留在领域层；文件打开由应用层的 load_rules 负责。
std::vector<FaultRule> parse_rules(std::string_view text);
std::vector<FaultRule> load_rules(const std::filesystem::path& path);
// 一次恢复动作的值类型请求；path 是动作目标（如 PWM 节点），由规则携带而非从设备查出。
struct RecoveryRequest { std::string rule; std::string sensor; std::string action; std::uint64_t sequence; std::string path; };
// 恢复动作的结局：accepted 表示策略层受理，success 表示执行器成功，detail 说明拒绝或失败原因。
struct RecoveryResult { std::string rule; std::string sensor; std::string action; bool accepted; bool success; unsigned attempts; std::string detail; };
// SEL 的内存态记录；time_ms 为系统时钟毫秒，value 为空表示该记录不带测量值。
struct SelRecord { std::uint64_t id; std::int64_t time_ms; std::string source; std::string state; std::string message; std::optional<double> value; };
// 启动重放时发现尾部残缺记录的处置方式：
//   refuse —— 只追加不回退，残缺行留在文件中间（无法修复但绝不丢数据）；
//   tail   —— 从残缺记录起点截断后追加（默认；对日志文件通常更正确）；
//   prepare—— 内存模式：记录只进内存，不写入文件也不重写压缩，用于测试与只读检查。
enum class Truncate { refuse, tail, prepare };
// SEL 用常驻描述符追加写入；记录在内存与文件中同时只保留最近 max_records 条。
// 每次 append 都尝试落盘，但只有缓冲区达到 batch_bytes 或记录属于"关键"（状态迁移/恢复）时才
// 真正 write+fdatasync，从而把每行一次系统调用降为每批一次，同时不牺牲故障证据的持久性。
class SelStore {
public:
    explicit SelStore(std::filesystem::path path, std::size_t max_records = 4096, Truncate policy = Truncate::tail,
                      std::function<int(int)> sync = {});
    ~SelStore();
    SelStore(const SelStore&) = delete;
    SelStore& operator=(const SelStore&) = delete;
    // important=true 表示该记录是故障证据，应立即落盘。
    std::uint64_t append(const std::string& source, const std::string& state, const std::string& message,
                         std::optional<double> value = std::nullopt, bool important = false);
    std::vector<SelRecord> query(std::size_t limit = 100) const;
    // 下一条要分配的记录 id；调用方可据此判断新记录是否已经写入。
    std::uint64_t next_id() const;
    // 把缓冲区写入磁盘并 fdatasync；缓冲区仍非空时返回 false。
    bool flush();
    // 处于 pending 状态（尚未成功落盘）的字节数。
    std::size_t pending_bytes() const;
    // 累计写入失败次数与因失败被丢弃的字节数，用于上报降级状态。
    std::uint64_t write_failures() const;
    std::uint64_t dropped_bytes() const;
    // fdatasync 失败次数。设备节点等不支持同步的路径会持续失败，因此单独计数，
    // 不与写入失败混淆：写到页缓存仍可能被内核落盘。
    std::uint64_t sync_failures() const;
    // 启动时因尾部残缺而被丢弃的字节数；仅在允许截断时文件会被修复。
    std::uint64_t truncated_bytes() const;
    // 未落盘缓冲区的上限；超过后丢弃最旧的未落盘字节。生产默认 1 MiB，测试可调小。
    void set_pending_cap(std::size_t bytes);
    // 让写失败立即返回而不是阻塞。管道等目标在满的时候会阻塞，开启后便于测试与快速降级。
    void set_nonblocking(bool enabled);
    // 仅在文件不存在时创建（O_CREAT|O_EXCL），用于测试与探测路径不可写的场景。
    static void write_probe(const std::filesystem::path& path);
private:
    void open();
    void replay();
    void trim();
    bool drain(bool sync);
    void drop_oldest(std::size_t count);
    std::filesystem::path path_;
    std::size_t max_records_;
    Truncate policy_;
    mutable std::mutex mutex_;
    std::vector<SelRecord> records_;
    std::string pending_;
    std::uint64_t next_id_ = 1;
    std::uint64_t write_failures_ = 0;
    std::uint64_t sync_failures_ = 0;
    std::uint64_t dropped_bytes_ = 0;
    std::uint64_t truncated_bytes_ = 0;
    int descriptor_ = -1;
    // 只有普通文件才尝试 fdatasync；设备节点对 fsync/fdatasync 返回 EINVAL。
    bool syncable_ = false;
    std::function<int(int)> sync_;
    std::size_t batch_bytes_ = 4096;
    std::size_t pending_cap_ = 1u << 20;
};
// 恢复策略：按 传感器+动作 施加冷却窗口、并发上限与有限重试。执行器在锁外调用，
// 因此慢动作不会阻塞其他传感器；策略非法（空执行器、零并发等）时构造抛出 std::invalid_argument。
class RecoveryPolicyEngine {
public:
    using Executor = std::function<bool(const RecoveryRequest&)>;
    RecoveryPolicyEngine(Executor executor, std::chrono::milliseconds cooldown = std::chrono::seconds(30), unsigned max_attempts = 3, std::size_t concurrency = 1);
    // 被冷却、并发或状态表容量拒绝时仍返回结果（accepted=false）；仅状态表已满且无法回收时
    // 返回 std::nullopt。执行器抛出的异常被折算为 success=false，不会向调用方传播。
    std::optional<RecoveryResult> submit(const RecoveryRequest& request);
    void reset(const std::string& sensor);
    std::size_t runtime_size() const;
private:
    struct State { std::chrono::steady_clock::time_point last; bool running = false; };
    Executor executor_;
    std::chrono::milliseconds cooldown_;
    unsigned max_attempts_;
    std::size_t concurrency_;
    std::size_t active_ = 0;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, State> states_;
};
// 单路传感器的判级状态机：阈值判级 + 迟滞 + 去抖，读取失败按 failure_limit 累进到 unavailable。
class Engine {
public:
    // 构造时调用 validate()，因此非法配置会抛出 std::invalid_argument。
    explicit Engine(Config config);
    // 喂入一个采样（空值表示读取失败）。仅当状态真正迁移时返回 Event，
    // 其余情况返回 std::nullopt，调用方无需再去重。
    std::optional<Event> update(std::optional<double> sample);
    // 最近一次已确认的状态（含迟滞与去抖后的结果）。
    State state() const;
private:
    State classify(double value) const;
    Config config_;
    State state_ = State::normal;
    State pending_ = State::normal;
    unsigned pending_count_ = 0;
    unsigned failure_count_ = 0;
    std::uint64_t sequence_ = 0;
};
// 单个测量量的取值来源（sysfs / i2c / gpio / mock）。读失败返回 std::nullopt。
class Reader {
public:
    virtual ~Reader() = default;
    virtual std::optional<double> read() = 0;
};
// 按配置构造读取器与设备；backend/path 非法时二者都抛出 std::invalid_argument。
std::unique_ptr<Reader> make_reader(const Config& config, LinuxIo& io);
std::unique_ptr<Device> make_device(const Config& config, LinuxIo& io);
// 监控逻辑只依赖这个接口，因此测试可以传入记录型实现（见 tests/runtime_management_test.cpp 的 HealthLogger）
// 而不必创建真实日志文件；生产实现是下面的 Logger。
class EventLogger {
public:
    virtual ~EventLogger() = default;
    // 追加一条状态迁移记录。写入失败只计数不抛出（同名用例已固化该行为）；
    // 只有轮转后重开文件失败才会抛出 std::system_error。
    virtual void write(const Event& event) = 0;
    // 追加一条动作/规则结局记录。这与状态迁移分开，SEL 才能区分"发生了什么"与"做了什么"。
    virtual void action(const std::string& id, const std::string& result) = 0;
    // 非持久日志实现默认没有 I/O 降级；Monitor 不需要识别具体 Logger 类型。
    virtual std::uint64_t write_failures() const { return 0; }
    virtual std::uint64_t dropped_bytes() const { return 0; }
};
// 日志写入策略。日志的故障证据价值高于采样末端的 SEL，但仍允许在崩溃时丢掉少量尾部行，
// 因此默认批量合并写入与不 fsync，需要更强保证时可显式打开。
struct LogPolicy {
    // 累积到该字节数就落盘，避免每行一次 write。
    std::size_t batch_bytes = 4096;
    // 每次落盘后是否 fsync（默认关闭：崩溃可能丢最后一批）。
    bool sync = false;
    // 未落盘缓冲区的上限，超过后丢弃最旧内容并计数。
    std::size_t pending_cap = 1u << 20;
};
// 日志写入的文件实现：批量落盘、按大小轮转、保留有限份数，落盘失败按降级计数上报。
class Logger : public EventLogger {
public:
    // sync=true 时每条记录立即落盘并 fsync，等价于旧行为，用于需要强持久的场景。
    // limit 是单文件字节上限，keep 是保留的历史份数；策略非法抛出 std::invalid_argument，
    // 日志文件打不开抛出 std::system_error。
    Logger(std::filesystem::path path, std::uintmax_t limit = 1048576, unsigned keep = 3,
           bool sync = false, std::size_t batch_bytes = 4096);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    void write(const Event& event) override;
    void action(const std::string& id, const std::string& result) override;
    // 把未落盘缓冲区写入磁盘；缓冲区仍非空时返回 false。
    bool flush();
    std::size_t pending_bytes() const;
    std::uint64_t write_failures() const override;
    std::uint64_t dropped_bytes() const override;
private:
    void append(const std::string& line);
    bool rotate(std::size_t incoming);
    void open();
    bool drain(bool sync);
    // 返回是否真的丢弃了最旧内容（只有丢弃时才计一次失败）。
    bool drop_oldest_excess();
    std::filesystem::path path_;
    std::uintmax_t limit_;
    unsigned keep_;
    LogPolicy policy_;
    std::string pending_;
    std::uint64_t write_failures_ = 0;
    std::uint64_t dropped_bytes_ = 0;
    int descriptor_ = -1;
    mutable std::mutex mutex_;
};
// 有界优先队列的线程池。任务按 priority 降序出队，同优先级保持先进先出；
// 任务抛出的异常被吞掉，只反映在 failed 计数里。capacity 为 0、线程数越界或线程创建
// 失败时构造抛出 std::invalid_argument。stop() 幂等，并会执行队列中已接受的任务。
class Worker {
public:
    struct Stats { std::uint64_t accepted; std::uint64_t completed; std::uint64_t failed; std::uint64_t rejected; std::size_t queued; std::size_t running; };
    explicit Worker(std::size_t capacity = 64, std::size_t threads = 1);
    ~Worker();
    // 空任务、已停止或队列满都返回 false（并计入 rejected），调用方需自行决定重试或丢弃。
    bool submit(std::function<void()> task, int priority = 0);
    Stats stats() const;
    void stop();
private:
    void run();
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    struct Task { std::function<void()> execute; int priority; };
    std::deque<Task> queue_;
    std::uint64_t accepted_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t failed_ = 0;
    std::uint64_t rejected_ = 0;
    std::size_t running_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};
// 以文本方式把 PWM 占空比写到 path；值超出 0..255 抛出 std::invalid_argument，
// 打开或写入失败抛出 std::system_error 或 std::runtime_error。
void write_pwm(const std::string& path, unsigned value, LinuxIo& io = system_io());
}
