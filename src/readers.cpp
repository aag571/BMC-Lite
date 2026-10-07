#include "bmc/core.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <cstring>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <ctime>

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
class GpioReader final : public Reader {
public:
    explicit GpioReader(const Config& config) : scale_(config.scale) {
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
        Fd chip(::open(parts[0].c_str(), O_RDONLY | O_CLOEXEC));
        if (chip.get() < 0) system_failure("open GPIO chip");
        gpiochip_info info {};
        if (::ioctl(chip.get(), GPIO_GET_CHIPINFO_IOCTL, &info) < 0) system_failure("query GPIO chip");
        if (offset >= info.lines) throw std::invalid_argument("GPIO offset outside chip");
        gpio_v2_line_request request {};
        request.offsets[0] = static_cast<__u32>(offset);
        request.num_lines = 1;
        request.config.flags = GPIO_V2_LINE_FLAG_INPUT;
        if (parts.size() == 3) request.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
        std::strncpy(request.consumer, "bmc-lite", sizeof(request.consumer) - 1);
        if (::ioctl(chip.get(), GPIO_V2_GET_LINE_IOCTL, &request) < 0) system_failure("request GPIO v2 input");
        line_ = Fd(request.fd);
        if (::fcntl(line_.get(), F_SETFD, FD_CLOEXEC) < 0) system_failure("set GPIO close-on-exec");
    }
    std::optional<double> read() override {
        gpio_v2_line_values values {};
        values.mask = 1;
        if (::ioctl(line_.get(), GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) return std::nullopt;
        return (values.bits & 1) ? scale_ : 0.0;
    }
private:
    Fd line_;
    double scale_;
};
class AdapterDevice final : public Device {
public:
    explicit AdapterDevice(const Config& config)
        : config_(config), info_{config.id, config.backend, config.path, "BMC sensor adapter", true, !config.action_path.empty()} {}
    const DeviceInfo& info() const override { return info_; }
    DeviceState state() const override { return state_; }
    bool open() override {
        if (state_ == DeviceState::ready) return true;
        if (config_.backend == "sysfs" && !std::filesystem::exists(config_.path)) {
            state_ = DeviceState::failed;
            return false;
        }
        try {
            reader_ = make_reader(config_);
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
            write_pwm(config_.action_path, static_cast<unsigned>(value));
            return true;
        } catch (const std::exception&) {
            state_ = DeviceState::degraded;
            return false;
        }
    }
private:
    Config config_;
    DeviceInfo info_;
    DeviceState state_ = DeviceState::closed;
    std::unique_ptr<Reader> reader_;
};
}
std::unique_ptr<Reader> make_reader(const Config& config) {
    validate(config);
    if (config.backend == "gpio") {
        return std::make_unique<GpioReader>(config);
    }
    if (config.backend == "mock") {
        return std::make_unique<MockReader>(config);
    }
    if (config.backend == "sysfs") {
        return std::make_unique<SysfsReader>(config);
    }
    return std::make_unique<I2cReader>(config);
}
std::unique_ptr<Device> make_device(const Config& config) {
    validate(config);
    return std::make_unique<AdapterDevice>(config);
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
