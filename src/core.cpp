#include "bmc/core.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace bmc {
namespace {
[[noreturn]] void system_failure(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
double number(const std::string& token) {
    std::size_t consumed = 0;
    const double value = std::stod(token, &consumed);
    if (consumed != token.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid number: " + token);
    }
    return value;
}
std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> result;
    std::istringstream input(text);
    std::string token;
    while (std::getline(input, token, separator)) {
        result.push_back(token);
    }
    return result;
}
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(milliseconds);
}
class MockReader final : public Reader {
public:
    explicit MockReader(const Config& config) : scale_(config.scale) {
        for (const auto& token : split(config.path, ',')) {
            values_.push_back(token == "err" ? std::nullopt : std::optional<double>(number(token)));
        }
        if (values_.empty()) {
            throw std::invalid_argument("empty mock sequence");
        }
    }
    std::optional<double> read() override {
        const auto value = values_[position_];
        position_ = (position_ + 1) % values_.size();
        return value ? std::optional<double>(*value * scale_) : std::nullopt;
    }
private:
    std::vector<std::optional<double>> values_;
    std::size_t position_ = 0;
    double scale_;
};
class SysfsReader final : public Reader {
public:
    explicit SysfsReader(const Config& config) : path_(config.path), scale_(config.scale) {}
    std::optional<double> read() override {
        std::ifstream input(path_);
        std::string token;
        std::string extra;
        if (!(input >> token) || (input >> extra)) {
            return std::nullopt;
        }
        try {
            const double value = number(token) * scale_;
            return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
private:
    std::string path_;
    double scale_;
};
class I2cReader final : public Reader {
public:
    explicit I2cReader(const Config& config) : scale_(config.scale) {
        const auto parts = split(config.path, ',');
        if (parts.size() != 3) {
            throw std::invalid_argument("i2c path requires device,address,register");
        }
        std::size_t address_end = 0;
        std::size_t register_end = 0;
        const auto address = std::stoul(parts[1], &address_end, 0);
        const auto register_value = std::stoul(parts[2], &register_end, 0);
        if (address_end != parts[1].size() || register_end != parts[2].size() || address > 0x7f || register_value > 0xff) {
            throw std::invalid_argument("invalid i2c address or register");
        }
        register_ = static_cast<__u8>(register_value);
        device_ = Fd(::open(parts[0].c_str(), O_RDWR | O_CLOEXEC));
        if (device_.get() < 0) {
            system_failure("open i2c device");
        }
        if (::ioctl(device_.get(), I2C_SLAVE, address) < 0) {
            system_failure("select i2c slave");
        }
        unsigned long functions = 0;
        if (::ioctl(device_.get(), I2C_FUNCS, &functions) < 0) {
            system_failure("query i2c capabilities");
        }
        if ((functions & I2C_FUNC_SMBUS_READ_WORD_DATA) == 0) {
            throw std::runtime_error("adapter does not support SMBus word reads");
        }
    }
    std::optional<double> read() override {
        union i2c_smbus_data data {};
        struct i2c_smbus_ioctl_data request {};
        request.read_write = I2C_SMBUS_READ;
        request.command = register_;
        request.size = I2C_SMBUS_WORD_DATA;
        request.data = &data;
        if (::ioctl(device_.get(), I2C_SMBUS, &request) < 0) {
            return std::nullopt;
        }
        const double value = static_cast<double>(data.word) * scale_;
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    }
private:
    Fd device_;
    __u8 register_ = 0;
    double scale_;
};
}
std::string name(State state) {
    switch (state) {
    case State::normal: return "normal";
    case State::warning: return "warning";
    case State::critical: return "critical";
    case State::unavailable: return "unavailable";
    }
    throw std::invalid_argument("invalid state");
}
std::string escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec;
            } else {
                output << character;
            }
        }
    }
    return output.str();
}
Fd::Fd(int value) : value_(value) {}
Fd::~Fd() {
    if (value_ >= 0) {
        ::close(value_);
    }
}
Fd::Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        if (value_ >= 0) {
            ::close(value_);
        }
        value_ = std::exchange(other.value_, -1);
    }
    return *this;
}
int Fd::get() const { return value_; }
void validate(const Config& config) {
    if (config.id.empty() || config.path.empty()) {
        throw std::invalid_argument("sensor id/path cannot be empty");
    }
    if (config.backend != "mock" && config.backend != "sysfs" && config.backend != "i2c") {
        throw std::invalid_argument("unknown backend: " + config.backend);
    }
    if (!std::isfinite(config.scale) || config.scale == 0 || !std::isfinite(config.warning) || !std::isfinite(config.critical) || !std::isfinite(config.hysteresis)) {
        throw std::invalid_argument("non-finite sensor policy");
    }
    const bool ordered = config.high ? config.warning < config.critical : config.warning > config.critical;
    if (!ordered || config.hysteresis < 0 || config.hysteresis >= std::abs(config.critical - config.warning) || config.debounce == 0 || config.failure_limit == 0) {
        throw std::invalid_argument("invalid sensor thresholds/windows");
    }
}
std::vector<Config> load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open config: " + path.string());
    }
    std::vector<Config> result;
    std::set<std::string> ids;
    std::string line;
    unsigned line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos || line[line.find_first_not_of(" \t\r")] == '#') {
            continue;
        }
        Config config;
        std::string direction;
        std::string trailing;
        std::string debounce;
        std::string failures;
        std::istringstream fields(line);
        if (!(fields >> config.id >> config.backend >> config.path >> config.scale >> direction >> config.warning >> config.critical >> config.hysteresis >> debounce >> failures >> config.action_path) || (fields >> trailing)) {
            throw std::invalid_argument("invalid config line " + std::to_string(line_number));
        }
        const auto parse_window = [](const std::string& token) {
            if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char character) { return character >= '0' && character <= '9'; })) {
                throw std::invalid_argument("invalid window");
            }
            const auto value = std::stoul(token);
            if (value == 0 || value > 1000000) {
                throw std::invalid_argument("window outside supported range");
            }
            return static_cast<unsigned>(value);
        };
        config.debounce = parse_window(debounce);
        config.failure_limit = parse_window(failures);
        if (direction != "high" && direction != "low") {
            throw std::invalid_argument("invalid threshold direction");
        }
        config.high = direction == "high";
        if (config.action_path == "-") {
            config.action_path.clear();
        }
        validate(config);
        if (!ids.insert(config.id).second) {
            throw std::invalid_argument("duplicate sensor: " + config.id);
        }
        result.push_back(config);
    }
    if (result.empty()) {
        throw std::invalid_argument("no sensors configured");
    }
    return result;
}
Engine::Engine(Config config) : config_(std::move(config)) { validate(config_); }
State Engine::state() const { return state_; }
State Engine::classify(double value) const {
    const double direction = config_.high ? 1.0 : -1.0;
    const double normalized = value * direction;
    const double warning = config_.warning * direction;
    const double critical = config_.critical * direction;
    if (normalized >= critical || (state_ == State::critical && normalized >= critical - config_.hysteresis)) {
        return State::critical;
    }
    if (normalized >= warning || ((state_ == State::warning || state_ == State::critical) && normalized >= warning - config_.hysteresis)) {
        return State::warning;
    }
    return State::normal;
}
std::optional<Event> Engine::update(std::optional<double> sample) {
    State desired;
    if (!sample || !std::isfinite(*sample)) {
        sample.reset();
        pending_count_ = 0;
        failure_count_ = std::min(failure_count_ + 1, config_.failure_limit);
        if (failure_count_ < config_.failure_limit || state_ == State::unavailable) {
            return std::nullopt;
        }
        desired = State::unavailable;
    } else {
        failure_count_ = 0;
        desired = classify(*sample);
        if (desired == state_) {
            pending_count_ = 0;
            return std::nullopt;
        }
        if (pending_count_ == 0 || pending_ != desired) {
            pending_ = desired;
            pending_count_ = 1;
        } else {
            pending_count_ = std::min(pending_count_ + 1, config_.debounce);
        }
        if (pending_count_ < config_.debounce) {
            return std::nullopt;
        }
    }
    Event event {config_.id, state_, desired, sample, sample ? "threshold" : "read_failure", ++sequence_};
    state_ = desired;
    pending_count_ = 0;
    return event;
}
std::unique_ptr<Reader> make_reader(const Config& config) {
    validate(config);
    if (config.backend == "mock") {
        return std::make_unique<MockReader>(config);
    }
    if (config.backend == "sysfs") {
        return std::make_unique<SysfsReader>(config);
    }
    return std::make_unique<I2cReader>(config);
}
Logger::Logger(std::filesystem::path path, std::uintmax_t limit, unsigned keep)
    : path_(std::move(path)), limit_(limit), keep_(keep) {
    if (limit == 0 || keep == 0 || keep > 100) {
        throw std::invalid_argument("invalid rotation policy");
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
}
void Logger::rotate(std::size_t incoming) {
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) + incoming <= limit_) {
        return;
    }
    for (unsigned index = keep_; index > 0; --index) {
        const auto destination = std::filesystem::path(path_.string() + "." + std::to_string(index));
        const auto source = index == 1 ? path_ : std::filesystem::path(path_.string() + "." + std::to_string(index - 1));
        if (std::filesystem::exists(destination)) {
            std::filesystem::remove(destination);
        }
        if (std::filesystem::exists(source)) {
            std::filesystem::rename(source, destination);
        }
    }
}
void Logger::append(const std::string& line) {
    std::lock_guard lock(mutex_);
    rotate(line.size() + 1);
    std::ofstream output(path_, std::ios::app);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << line << '\n';
    output.flush();
}
void Logger::write(const Event& event) {
    std::ostringstream output;
    output << std::setprecision(17) << "{\"time_ms\":" << timestamp()
           << ",\"sensor\":\"" << escape(event.id)
           << "\",\"before\":\"" << name(event.before)
           << "\",\"state\":\"" << name(event.after)
           << "\",\"value\":";
    if (event.value && std::isfinite(*event.value)) {
        output << *event.value;
    } else {
        output << "null";
    }
    output << ",\"reason\":\"" << escape(event.reason) << "\",\"sequence\":" << event.sequence << '}';
    append(output.str());
}
void Logger::action(const std::string& id, const std::string& result) {
    append("{\"time_ms\":" + timestamp() + ",\"sensor\":\"" + escape(id) + "\",\"action\":\"" + escape(result) + "\"}");
}
Worker::Worker(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("zero queue capacity");
    }
    thread_ = std::thread(&Worker::run, this);
}
Worker::~Worker() { stop(); }
bool Worker::submit(std::function<void()> task) {
    std::lock_guard lock(mutex_);
    if (!task || stopping_ || queue_.size() >= capacity_) {
        return false;
    }
    queue_.push_back(std::move(task));
    wake_.notify_one();
    return true;
}
void Worker::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}
void Worker::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}
void write_pwm(const std::string& path, unsigned value) {
    if (value > 255) {
        throw std::invalid_argument("PWM value out of range");
    }
    Fd descriptor(::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.get() < 0) {
        system_failure("open PWM");
    }
    const std::string text = std::to_string(value) + "\n";
    const auto count = ::write(descriptor.get(), text.data(), text.size());
    if (count < 0) {
        system_failure("write PWM");
    }
    if (static_cast<std::size_t>(count) != text.size()) {
        throw std::runtime_error("short PWM write");
    }
}
}
