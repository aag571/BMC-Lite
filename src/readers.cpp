#include "bmc/chip.hpp"
#include "bmc/core.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

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
// ioctl 的第三个参数在 I2C_SLAVE 是整数、在其余调用是结构体指针，统一经 void* 传递。
void* as_ioctl_argument(unsigned long value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}
// i2c 一类后端共用的地址解析：<device>,<address>，7 位地址。
std::uint8_t parse_address(const std::string& token) {
    std::size_t end = 0;
    const auto address = std::stoul(token, &end, 0);
    if (end != token.size() || address > 0x7f) {
        throw std::invalid_argument("invalid i2c address");
    }
    return static_cast<std::uint8_t>(address);
}
}
namespace {
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
class RawI2cReader final : public Reader {
public:
    explicit RawI2cReader(const Config& config, LinuxIo& io) : io_(io), scale_(config.scale) {
        const auto parts = split(config.path, ',');
        if (parts.size() != 3) {
            throw std::invalid_argument("i2c path requires device,address,register");
        }
        std::size_t register_end = 0;
        const auto register_value = std::stoul(parts[2], &register_end, 0);
        if (register_end != parts[2].size() || register_value > 0xff) {
            throw std::invalid_argument("invalid i2c register");
        }
        const auto address = parse_address(parts[1]);
        register_ = static_cast<std::uint8_t>(register_value);
        device_ = Fd(io_.open(parts[0], O_RDWR | O_CLOEXEC));
        if (device_.get() < 0) {
            system_failure("open i2c device");
        }
        if (io_.ioctl(device_.get(), I2C_SLAVE, as_ioctl_argument(address)) < 0) {
            system_failure("select i2c slave");
        }
        unsigned long functions = 0;
        if (io_.ioctl(device_.get(), I2C_FUNCS, &functions) < 0) {
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
        if (io_.ioctl(device_.get(), I2C_SMBUS, &request) < 0) {
            return std::nullopt;
        }
        const double value = static_cast<double>(data.word) * scale_;
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    }
private:
    LinuxIo& io_;
    Fd device_;
    std::uint8_t register_ = 0;
    double scale_;
};
// 专用芯片读取：由 ChipDriver 负责寄存器语义与换算，Reader 只负责把结果交给上层。
// 后端的 chip 部分可以写成 "lm75" 或 "lm75@bus"（@ 后为测量量），默认取该型号的第一个测量量。
class ChipReader final : public Reader {
public:
    explicit ChipReader(const Config& config, LinuxIo& io) : scale_(config.scale) {
        const auto parts = split(config.path, ',');
        if (parts.size() != 2 || parts[0].empty()) {
            throw std::invalid_argument("chip path requires device,address");
        }
        const std::string backend = config.backend.substr(4);
        const std::size_t at = backend.find('@');
        const std::string chip = backend.substr(0, at);
        std::string feature = at == std::string::npos ? std::string() : backend.substr(at + 1);
        bus_ = std::make_unique<I2cBus>(parts[0], parse_address(parts[1]), io);
        driver_ = make_chip(chip, *bus_, feature);
        const auto features = driver_->features();
        if (feature.empty()) {
            if (features.empty()) {
                throw std::invalid_argument("chip has no features: " + chip);
            }
            feature_ = features.front().name;
        } else {
            feature_ = std::move(feature);
        }
    }
    std::optional<double> read() override {
        const auto value = driver_->read(feature_);
        if (!value) {
            return std::nullopt;
        }
        const double scaled = *value * scale_;
        return std::isfinite(scaled) ? std::optional<double>(scaled) : std::nullopt;
    }
private:
    std::unique_ptr<I2cBus> bus_;
    std::unique_ptr<ChipDriver> driver_;
    std::string feature_;
    double scale_;
};
class GpioReader final : public Reader {
public:
    explicit GpioReader(const Config& config, LinuxIo& io) : io_(io), scale_(config.scale) {
        const auto parts = split(config.path, ',');
        if (parts.size() < 2 || parts.size() > 3 || parts[0].empty()) {
            throw std::invalid_argument("gpio path requires chip,offset[,active-low]");
        }
        if (parts[1].empty() || !std::all_of(parts[1].begin(), parts[1].end(), [](unsigned char character) {
            return character >= '0' && character <= '9';
        })) {
            throw std::invalid_argument("invalid GPIO line offset");
        }
        const auto offset = std::stoul(parts[1]);
        if (offset > UINT32_MAX || (parts.size() == 3 && parts[2] != "active-low")) {
            throw std::invalid_argument("invalid GPIO options");
        }
        Fd chip(io_.open(parts[0], O_RDONLY | O_CLOEXEC));
        if (chip.get() < 0) system_failure("open GPIO chip");
        gpiochip_info info {};
        if (io_.ioctl(chip.get(), GPIO_GET_CHIPINFO_IOCTL, &info) < 0) system_failure("query GPIO chip");
        if (offset >= info.lines) throw std::invalid_argument("GPIO offset outside chip");
        gpio_v2_line_request request {};
        request.offsets[0] = static_cast<__u32>(offset);
        request.num_lines = 1;
        request.config.flags = GPIO_V2_LINE_FLAG_INPUT;
        if (parts.size() == 3) request.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
        std::strncpy(request.consumer, "bmc-lite", sizeof(request.consumer) - 1);
        if (io_.ioctl(chip.get(), GPIO_V2_GET_LINE_IOCTL, &request) < 0) system_failure("request GPIO v2 input");
        line_ = Fd(request.fd);
        // line fd 由 RAII 管理。Fake 可返回自己打开的普通 fd，并脚本化后续 ioctl；
        // 因此关闭与 CLOEXEC 也可测试，电气行为仍需实际 GPIO 设备验证。
        if (::fcntl(line_.get(), F_SETFD, FD_CLOEXEC) < 0) system_failure("set GPIO close-on-exec");
    }
    std::optional<double> read() override {
        gpio_v2_line_values values {};
        values.mask = 1;
        if (io_.ioctl(line_.get(), GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) return std::nullopt;
        return (values.bits & 1) ? scale_ : 0.0;
    }
private:
    LinuxIo& io_;
    Fd line_;
    double scale_;
};
class AdapterDevice final : public Device {
public:
    AdapterDevice(const Config& config, LinuxIo& io)
        : io_(io), config_(config), info_{config.id, config.backend, config.path, "BMC sensor adapter", true, !config.action_path.empty()} {}
    const DeviceInfo& info() const override { return info_; }
    DeviceState state() const override { return state_; }
    bool open() override {
        if (state_ == DeviceState::ready) return true;
        if (config_.backend == "sysfs" && !std::filesystem::exists(config_.path)) {
            state_ = DeviceState::failed;
            return false;
        }
        try {
            reader_ = make_reader(config_, io_);
        } catch (const std::exception&) {
            state_ = DeviceState::failed;
            return false;
        }
        state_ = DeviceState::ready;
        return true;
    }
    void close() noexcept override { reader_.reset(); state_ = DeviceState::closed; }
    bool probe() override {
        if (!open()) return false;
        const auto sample = reader_->read();
        if (!sample) state_ = DeviceState::degraded;
        return sample.has_value();
    }
    std::optional<double> read_value() override {
        if (!reader_ && !open()) return std::nullopt;
        const auto value = reader_->read();
        if (!value) state_ = DeviceState::degraded;
        else state_ = DeviceState::ready;
        return value;
    }
    bool write_value(double value) override {
        if (!info_.writable || !std::isfinite(value) || value < 0 || value > 255 || std::floor(value) != value) return false;
        try {
            // 设备写走注入的 io，与 PwmAction 的路径保持同一实现。
            write_pwm(config_.action_path, static_cast<unsigned>(value), io_);
            return true;
        } catch (const std::exception&) {
            state_ = DeviceState::degraded;
            return false;
        }
    }
private:
    LinuxIo& io_;
    Config config_;
    DeviceInfo info_;
    DeviceState state_ = DeviceState::closed;
    std::unique_ptr<Reader> reader_;
};
}
std::unique_ptr<Reader> make_reader(const Config& config, LinuxIo& io) {
    validate(config);
    if (config.backend == "gpio") {
        return std::make_unique<GpioReader>(config, io);
    }
    if (config.backend == "mock") {
        return std::make_unique<MockReader>(config);
    }
    if (config.backend == "sysfs") {
        return std::make_unique<SysfsReader>(config);
    }
    if (config.backend == "i2c") {
        return std::make_unique<RawI2cReader>(config, io);
    }
    // 其余 i2c:<chip>[@feature] 后端走专用芯片驱动。
    return std::make_unique<ChipReader>(config, io);
}
std::unique_ptr<Device> make_device(const Config& config, LinuxIo& io) {
    validate(config);
    return std::make_unique<AdapterDevice>(config, io);
}
void DeviceRegistry::add(std::unique_ptr<Device> device) {
    if (!device || device->info().id.empty() || find(device->info().id) != nullptr) {
        throw std::invalid_argument("invalid or duplicate device");
    }
    devices_.push_back(std::move(device));
}
Device* DeviceRegistry::find(const std::string& id) const {
    for (const auto& device : devices_) if (device->info().id == id) return device.get();
    return nullptr;
}
std::vector<DeviceInfo> DeviceRegistry::inventory() const {
    std::vector<DeviceInfo> result;
    result.reserve(devices_.size());
    for (const auto& device : devices_) result.push_back(device->info());
    return result;
}
bool DeviceRegistry::open_all() {
    bool success = true;
    for (const auto& device : devices_) if (!device->open()) success = false;
    return success;
}
void DeviceRegistry::close_all() noexcept {
    for (const auto& device : devices_) device->close();
}
}
