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

// readers.cpp —— 各采集后端（mock/sysfs/原始 I2C/GPIO/专用芯片）与设备注册（声明见 bmc/core.hpp）。
// 约束：设备访问全部经注入的 LinuxIo，路径解析与错误分支才可在 FakeLinuxIo 下测试；
// backend 与 path 语法一一对应，语法错误在 Reader 构造期抛 std::invalid_argument。
namespace bmc {
namespace {
// 把当前 errno 转成异常；必须在失败的系统调用之后立刻调用，中间不能插入可能改写 errno 的操作。
[[noreturn]] void system_failure(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
// 严格解析一个 double：必须整串被消费且结果有限，因此 "12abc"、"nan"、"inf" 都会被拒绝。
double number(const std::string& token) {
    std::size_t consumed = 0;
    const double value = std::stod(token, &consumed);
    if (consumed != token.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid number: " + token);
    }
    return value;
}
// 按单个分隔符切分，空字段保留（"a,,b" 得到 3 段）；路径里的空段会在各自的解析处报错，不会被悄悄跳过。
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
// unsigned long 与指针宽度在不同平台上不一定相同，先转 uintptr_t 再转指针，避免窄化告警与截断。
void* as_ioctl_argument(unsigned long value) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
}
// i2c 一类后端共用的地址解析：<device>,<address>，7 位地址。
// 基址 0 表示同时接受十进制与 0x 十六进制；大于 0x7f 或带尾随字符都拒绝。
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
// 测试后端，不接触任何设备。path 语法：逗号分隔的采样序列，如 "70,72,err"，
// 其中 err 表示该次读取失败（返回 nullopt）。序列按下标循环推进，读完后从头再来。
// scale 只作用于成功样本；空序列在构造期报错，避免读时越界。
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
    // 每次调用推进一位：即使本次返回 nullopt 也会推进，因此失败样本能被脚本化地复现。
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
// 通用 sysfs/hwmon 后端。path 是单个取值文件的绝对路径，如 /sys/class/hwmon/hwmon0/temp1_input，
// 只接受“恰好一个数值 token”：出现第二个空白分隔的 token 就按格式不符处理（尾随换行不算 token），
// 以免把 "70000 3" 这类多值文件读成单个有效读数。
class SysfsReader final : public Reader {
public:
    explicit SysfsReader(const Config& config) : path_(config.path), scale_(config.scale) {}
    // 打开失败、格式不符、非数值、结果非有限，一律返回 nullopt 且不抛异常，调用方据此记为无读数。
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
// 原始 SMBus word 读后端，不做任何芯片语义换算。path 语法：device,address,register，
// 如 /dev/i2c-1,0x48,0x00；address 为 7 位，register 为 8 位，都接受 0x 前缀。
// 描述符由 Fd 管理，析构即关闭。
class RawI2cReader final : public Reader {
public:
    // 构造时就检查适配器是否支持 I2C_FUNC_SMBUS_READ_WORD_DATA：本后端每轮只发这一种事务，
    // 能力缺失属于配置选错了设备，提前拒绝比让每次采样静默失败更容易定位。
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
    // 每次读取发一次 SMBus Read Word Data（command = register_），结果按主机字节序乘 scale。
    // ioctl 失败返回 nullopt，与其它后端保持相同的失败语义。
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
// 后端字符串为 "i2c:<chip>" 或 "i2c:<chip>@<feature>"（如 i2c:lm75b@temp），path 只接受 device,address。
// 型号、地址与测量量都在构造期校验：未知型号或不支持的测量量由 make_chip 抛 std::invalid_argument，
// 不会退化成运行期静默返回空值。
// 成员声明顺序是 bus_ 在 driver_ 之前，析构时 driver_ 先释放，驱动持有的 I2cBus& 不会悬空。
class ChipReader final : public Reader {
public:
    // 去掉 "i2c:" 前缀后按 '@' 拆出测量量；feature 为空时取型号声明的第一个测量量。
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
    // 驱动内部完成寄存器解码与单位换算，这里只做 scale 与有限性检查。
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
// GPIO v2 输入后端。path 语法：chip,offset[,active-low]，如 /dev/gpiochip0,17 或 /dev/gpiochip0,17,active-low；
// offset 必须是纯十进制数字（不接受 0x 前缀），且必须小于该 chip 的线数。
// 上报值为 scale_（逻辑值 1）或 0.0（逻辑值 0），因此 scale 通常取 1。
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
        // 用 GPIO v2 线请求而不是旧的 v1 handle：只有 v2 才能在同一请求里带上 ACTIVE_LOW 等线标志，
        // 并直接返回可读值的 line fd。
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
        // line fd 由内核新建，不保证带 CLOEXEC，必须自己补上，否则 exec 后子进程会继承这条 GPIO 线。
        if (::fcntl(line_.get(), F_SETFD, FD_CLOEXEC) < 0) system_failure("set GPIO close-on-exec");
    }
    // values.bits 的 bit0 即本线的逻辑值；本文件不自行取反，是否按 ACTIVE_LOW 翻转由请求标志决定
    // （该语义未在真实设备上验证）。ioctl 失败返回 nullopt。
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
// 把 Reader 包装成 Device，负责 open/close/probe/read_value/write_value 与 state_ 的映射；
// 读取本身仍在各 Reader 里，本类不新增设备语义。
//
// state_ 含义：closed=未打开或已关闭；ready=已打开且最近一次读取有值；
// degraded=能打开但读不到值（SMBus 无应答、sysfs 格式不符、PWM 写失败），下次读取仍可能恢复；
// failed=打不开（sysfs 路径不存在或 make_reader 抛异常），必须重新 open 才可能恢复。
class AdapterDevice final : public Device {
public:
    // DeviceInfo 在这里一次性填好；writable 由 action_path 是否为空决定（有 PWM 节点才允许写）。
    AdapterDevice(const Config& config, LinuxIo& io)
        : io_(io), config_(config), info_{config.id, config.backend, config.path, "BMC sensor adapter", true, !config.action_path.empty()} {}
    const DeviceInfo& info() const override { return info_; }
    DeviceState state() const override { return state_; }
    // 已 ready 直接成功；sysfs 先查路径是否存在；构造 Reader 的异常一律转成 failed，不向外抛。
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
    // noexcept：释放 Reader（其析构关闭持有的 fd）并把状态置回 closed，之后可以再次 open。
    void close() noexcept override { reader_.reset(); state_ = DeviceState::closed; }
    // 打开后试读一次；读不到只降级为 degraded，返回值表示本次试读是否有值。
    bool probe() override {
        if (!open()) return false;
        const auto sample = reader_->read();
        if (!sample) state_ = DeviceState::degraded;
        return sample.has_value();
    }
    // 未打开时先 open；read 返回 nullopt 会把状态置为 degraded 并原样返回 nullopt，绝不抛异常。
    std::optional<double> read_value() override {
        if (!reader_ && !open()) return std::nullopt;
        const auto value = reader_->read();
        if (!value) state_ = DeviceState::degraded;
        else state_ = DeviceState::ready;
        return value;
    }
    // 只针对有 action_path 的可写设备：要求 0..255 的整数；写失败降级为 degraded 并返回 false。
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
// 按 backend 分发：gpio/mock/sysfs/i2c 之外的字符串一律按 "i2c:<chip>[@feature]" 交给 ChipReader。
// 先 validate(config) 保证公共字段自洽，各 Reader 只需再校验自己那份 path 语法。
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
// 设备工厂：所有后端共用 AdapterDevice，差异都在它内部构造的 Reader 上。
std::unique_ptr<Device> make_device(const Config& config, LinuxIo& io) {
    validate(config);
    return std::make_unique<AdapterDevice>(config, io);
}
// 注册设备：空指针、空 id、重复 id 都抛 std::invalid_argument，保证表内 id 唯一。
void DeviceRegistry::add(std::unique_ptr<Device> device) {
    if (!device || device->info().id.empty() || find(device->info().id) != nullptr) {
        throw std::invalid_argument("invalid or duplicate device");
    }
    devices_.push_back(std::move(device));
}
// 线性查找；未找到返回 nullptr，调用方负责判空。
Device* DeviceRegistry::find(const std::string& id) const {
    for (const auto& device : devices_) if (device->info().id == id) return device.get();
    return nullptr;
}
// 返回 DeviceInfo 的副本快照，之后设备改状态不会影响已返回的列表。
std::vector<DeviceInfo> DeviceRegistry::inventory() const {
    std::vector<DeviceInfo> result;
    result.reserve(devices_.size());
    for (const auto& device : devices_) result.push_back(device->info());
    return result;
}
// 逐个 open 且不短路，保证每个设备都被尝试一次；返回是否全部成功。
bool DeviceRegistry::open_all() {
    bool success = true;
    for (const auto& device : devices_) if (!device->open()) success = false;
    return success;
}
// noexcept：退出路径上逐个 close，某个设备出错也不影响其余设备。
void DeviceRegistry::close_all() noexcept {
    for (const auto& device : devices_) device->close();
}
}
