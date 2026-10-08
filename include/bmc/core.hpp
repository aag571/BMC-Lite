#pragma once
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
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>

namespace bmc {
enum class DeviceState { closed, ready, degraded, failed };
struct DeviceInfo {
    std::string id;
    std::string kind;
    std::string path;
    std::string description;
    bool readable = false;
    bool writable = false;
};
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
class DeviceRegistry {
public:
    void add(std::unique_ptr<Device> device);
    Device* find(const std::string& id) const;
    std::vector<DeviceInfo> inventory() const;
    bool open_all();
    void close_all() noexcept;
private:
    std::vector<std::unique_ptr<Device>> devices_;
};
enum class State { normal, warning, critical, unavailable };
std::string name(State state);
std::string escape(const std::string& value);
class Fd {
public:
    explicit Fd(int value = -1);
    ~Fd();
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
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
double apply(const Calibration& calibration, double raw);
struct Config {
    std::string id;
    std::string backend;
    std::string path;
    double scale = 1;
    bool high = true;
    double warning = 70;
    double critical = 90;
    double hysteresis = 3;
    unsigned debounce = 3;
    unsigned failure_limit = 3;
    std::string action_path;
    Calibration calibration;
};
std::vector<Config> load_config(const std::filesystem::path& path);
void validate(const Config& config);
struct Event {
    std::string id;
    State before;
    State after;
    std::optional<double> value;
    std::string reason;
    std::uint64_t sequence;
};
enum class BusEventType { sensor_state, configuration, service, recovery };
struct BusEvent {
    BusEventType type;
    std::string source;
    std::string message;
    std::optional<double> value;
    std::uint64_t sequence = 0;
};
class EventBus {
public:
    using Handler = std::function<void(const BusEvent&)>;
    explicit EventBus(std::size_t capacity = 256);
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    std::uint64_t subscribe(BusEventType type, Handler handler);
    bool publish(BusEvent event);
    void stop();
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
struct FaultRule {
    std::string id;
    std::string sensor;
    State trigger = State::critical;
    unsigned confirmations = 1;
    unsigned clear_confirmations = 1;
    std::string action;
};
struct RuleDecision {
    std::string rule;
    std::string sensor;
    State state;
    bool active;
    std::string action;
    std::uint64_t sequence;
};
class FaultRuleEngine {
public:
    explicit FaultRuleEngine(std::vector<FaultRule> rules);
    std::vector<RuleDecision> evaluate(const Event& event);
    void reset();
private:
    struct Runtime { unsigned bad = 0; unsigned good = 0; bool active = false; std::uint64_t sequence = 0; };
    std::vector<FaultRule> rules_;
    std::unordered_map<std::string, Runtime> runtime_;
};
std::vector<FaultRule> load_rules(const std::filesystem::path& path);
struct RecoveryRequest { std::string rule; std::string sensor; std::string action; std::uint64_t sequence; std::string path; };
struct RecoveryResult { std::string rule; std::string sensor; std::string action; bool accepted; bool success; unsigned attempts; std::string detail; };
struct SelRecord { std::uint64_t id; std::int64_t time_ms; std::string source; std::string state; std::string message; std::optional<double> value; };
class SelStore {
public:
    explicit SelStore(std::filesystem::path path, std::size_t max_records = 4096);
    std::uint64_t append(const std::string& source, const std::string& state, const std::string& message, std::optional<double> value = std::nullopt);
    std::vector<SelRecord> query(std::size_t limit = 100) const;
    std::uint64_t next_id() const;
    void flush();
private:
    void load();
    std::filesystem::path path_;
    std::size_t max_records_;
    mutable std::mutex mutex_;
    std::vector<SelRecord> records_;
    std::uint64_t next_id_ = 1;
};
class RecoveryPolicyEngine {
public:
    using Executor = std::function<bool(const RecoveryRequest&)>;
    RecoveryPolicyEngine(Executor executor, std::chrono::milliseconds cooldown = std::chrono::seconds(30), unsigned max_attempts = 3, std::size_t concurrency = 1);
    std::optional<RecoveryResult> submit(const RecoveryRequest& request);
    void reset(const std::string& sensor);
private:
    struct State { std::chrono::steady_clock::time_point last; bool running = false; };
    Executor executor_;
    std::chrono::milliseconds cooldown_;
    unsigned max_attempts_;
    std::size_t concurrency_;
    std::size_t active_ = 0;
    std::mutex mutex_;
    std::unordered_map<std::string, State> states_;
};
class Engine {
public:
    explicit Engine(Config config);
    std::optional<Event> update(std::optional<double> sample);
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
class Reader {
public:
    virtual ~Reader() = default;
    virtual std::optional<double> read() = 0;
};
std::unique_ptr<Reader> make_reader(const Config& config, LinuxIo& io);
std::unique_ptr<Device> make_device(const Config& config, LinuxIo& io);
// 监控逻辑依赖日志接口；测试可实现 RecordingLogger，而不需要创建真实文件。
class EventLogger {
public:
    virtual ~EventLogger() = default;
    virtual void write(const Event& event) = 0;
    virtual void action(const std::string& id, const std::string& result) = 0;
};
class Logger : public EventLogger {
public:
    Logger(std::filesystem::path path, std::uintmax_t limit = 1048576, unsigned keep = 3);
    void write(const Event& event) override;
    void action(const std::string& id, const std::string& result) override;
private:
    void append(const std::string& line);
    void rotate(std::size_t incoming);
    std::filesystem::path path_;
    std::uintmax_t limit_;
    unsigned keep_;
    std::mutex mutex_;
};
class Worker {
public:
    struct Stats { std::uint64_t accepted; std::uint64_t completed; std::uint64_t failed; std::uint64_t rejected; std::size_t queued; std::size_t running; };
    explicit Worker(std::size_t capacity = 64, std::size_t threads = 1);
    ~Worker();
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
void write_pwm(const std::string& path, unsigned value, LinuxIo& io = system_io());
}
