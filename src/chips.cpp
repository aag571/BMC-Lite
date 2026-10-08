#include "bmc/chip.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bmc {
namespace {
[[noreturn]] void system_failure(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
// SMBus 传输的公共部分：填好 i2c_smbus_ioctl_data 后发出 ioctl。
bool smbus(int descriptor, LinuxIo& io, char operation, std::uint8_t command, int size, i2c_smbus_data& data) {
    i2c_smbus_ioctl_data request {};
    request.read_write = operation;
    request.command = command;
    request.size = size;
    request.data = &data;
    return io.ioctl(descriptor, I2C_SMBUS, &request) >= 0;
}
bool same_feature(const std::string& requested, const std::string& feature) {
    return requested == feature;
}
// 校验请求的测量量属于该型号；空名字表示使用默认量。
bool feature_supported(const std::string& requested, const std::vector<ChipFeature>& features) {
    if (requested.empty()) {
        return true;
    }
    return std::any_of(features.begin(), features.end(),
        [&requested](const ChipFeature& feature) { return feature.name == requested; });
}
}

std::uint16_t from_register_bytes(std::uint8_t high, std::uint8_t low) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8) | low);
}

std::int16_t to_signed(std::uint16_t value) {
    return static_cast<std::int16_t>(value);
}

I2cBus::I2cBus(const std::string& device, std::uint8_t address, LinuxIo& io) : address_(address), io_(&io) {
    descriptor_ = io_->open(device, O_RDWR | O_CLOEXEC);
    if (descriptor_ < 0) {
        system_failure("open i2c device");
    }
    if (io_->ioctl(descriptor_, I2C_SLAVE, reinterpret_cast<void*>(static_cast<std::uintptr_t>(address))) < 0) {
        system_failure("select i2c slave");
    }
    unsigned long functions = 0;
    if (io_->ioctl(descriptor_, I2C_FUNCS, &functions) < 0) {
        system_failure("query i2c capabilities");
    }
    const unsigned long required = I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA;
    if ((functions & required) != required) {
        throw std::runtime_error("adapter lacks required SMBus byte/word data support");
    }
}

I2cBus::~I2cBus() {
    if (descriptor_ >= 0 && io_ != nullptr) {
        io_->close(descriptor_);
    }
}

std::optional<std::uint8_t> I2cBus::read_byte(std::uint8_t command) {
    i2c_smbus_data data {};
    if (!smbus(descriptor_, *io_, I2C_SMBUS_READ, command, I2C_SMBUS_BYTE_DATA, data)) {
        return std::nullopt;
    }
    return data.byte;
}

std::optional<std::uint16_t> I2cBus::read_word(std::uint8_t command) {
    i2c_smbus_data data {};
    if (!smbus(descriptor_, *io_, I2C_SMBUS_READ, command, I2C_SMBUS_WORD_DATA, data)) {
        return std::nullopt;
    }
    // 内核已完成 SMBus 传输的字节序还原，data.word 即主机字节序数值。
    return static_cast<std::uint16_t>(data.word);
}

std::optional<std::vector<std::uint8_t>> I2cBus::read_block(std::uint8_t command, std::uint8_t count) {
    if (count == 0 || count > 32) {
        throw std::invalid_argument("i2c block length outside 1..32");
    }
    i2c_smbus_data data {};
    data.block[0] = count;
    if (!smbus(descriptor_, *io_, I2C_SMBUS_READ, command, I2C_SMBUS_I2C_BLOCK_DATA, data)) {
        return std::nullopt;
    }
    const auto length = std::min<std::uint8_t>(data.block[0], count);
    return std::vector<std::uint8_t>(data.block + 1, data.block + 1 + length);
}

bool I2cBus::write_byte(std::uint8_t command, std::uint8_t value) {
    i2c_smbus_data data {};
    data.byte = value;
    return smbus(descriptor_, *io_, I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA, data);
}

bool I2cBus::write_word(std::uint8_t command, std::uint16_t value) {
    i2c_smbus_data data {};
    data.word = value;
    return smbus(descriptor_, *io_, I2C_SMBUS_WRITE, command, I2C_SMBUS_WORD_DATA, data);
}

namespace {
// ---------------- LM75 / LM75A / LM75B 温度传感器 ----------------
// 温度寄存器 0x00，16 位、高字节在前的二进制补码。LM75A/B 低字节的 bit7..bit5 是 0.125 ℃ 位，
// 因此 11 位分辨率下 LSB = 0.125 ℃。原始 LM75 只有 0.5 ℃ 分辨率，低字节为 0。
class Lm75Driver final : public ChipDriver {
public:
    Lm75Driver(I2cBus& bus, bool high_resolution) : bus_(bus), high_resolution_(high_resolution) {}
    const char* name() const override { return high_resolution_ ? "lm75b" : "lm75"; }
    std::vector<ChipFeature> features() const override { return {{"temp", "C"}}; }
    std::optional<double> read(const std::string& feature) override {
        if (!same_feature(feature, "temp")) {
            return std::nullopt;
        }
        const auto raw = bus_.read_word(kTemperature);
        if (!raw) {
            return std::nullopt;
        }
        if (!high_resolution_) {
            // 9 位分辨率：bit8..bit0 是一个 0.5 ℃ 的二进制补码量，bit8 实际是 0.5 ℃ 位。
            const auto bits = static_cast<std::int16_t>(*raw) >> 7;
            return static_cast<double>(bits) * 0.5;
        }
        // 11 位分辨率：低字节 bit7..bit5 是 0.125 ℃ 位，整体右移 5 位后符号扩展。
        const auto bits = static_cast<std::int16_t>(*raw) >> 5;
        return static_cast<double>(bits) * 0.125;
    }

private:
    static constexpr std::uint8_t kTemperature = 0x00;
    I2cBus& bus_;
    bool high_resolution_ = true;
};

// ---------------- ADM1275 热插拔控制器（PMBus，DIRECT 格式） ----------------
// 该芯片只有 31 条命令：没有 READ_PIN，也没有温度命令，因此只提供 vin/vout/iout。
// READ_VIN 0x88 / READ_VOUT 0x8B / READ_IOUT 0x8C，数据右对齐在 bit[11:0]，无符号。
class Adm1275Driver final : public ChipDriver {
public:
    explicit Adm1275Driver(I2cBus& bus) : bus_(bus) {}
    const char* name() const override { return "adm1275"; }
    std::vector<ChipFeature> features() const override { return {{"vin", "V"}, {"vout", "V"}, {"iout", "A"}}; }
    std::optional<double> read(const std::string& feature) override {
        if (same_feature(feature, "vin")) {
            return code_to_volts(kReadVin);
        }
        if (same_feature(feature, "vout")) {
            return code_to_volts(kReadVout);
        }
        if (same_feature(feature, "iout")) {
            return code_to_amps(kReadIout);
        }
        return std::nullopt;
    }

private:
    // 数据手册的 LSB 法比舍入后的 m/b/R 系数更精确：V = LSB × (code + 0.5)。
    std::optional<double> code_to_volts(std::uint8_t command) const {
        const auto raw = bus_.read_word(command);
        if (!raw) {
            return std::nullopt;
        }
        const double code = static_cast<double>(*raw & 0x0FFF);
        return voltage_lsb_ * (code + 0.5);
    }
    // I = 12.4 uV × (code − 2048) / R_SENSE；未配置分流电阻时无法给出安培值。
    std::optional<double> code_to_amps(std::uint8_t command) const {
        if (!(shunt_milliohms_ > 0)) {
            return std::nullopt;
        }
        const auto raw = bus_.read_word(command);
        if (!raw) {
            return std::nullopt;
        }
        const double code = static_cast<double>(*raw & 0x0FFF);
        return 12.4e-6 * (code - 2048.0) / (shunt_milliohms_ * 1e-3);
    }
    static constexpr std::uint8_t kReadVin = 0x88;
    static constexpr std::uint8_t kReadVout = 0x8B;
    static constexpr std::uint8_t kReadIout = 0x8C;
    I2cBus& bus_;
    // ADM1275-1/-3 默认量程为 0–20 V（PMON_CONFIG VRANGE 默认 1）。
    double voltage_lsb_ = 5.208e-3;
    double shunt_milliohms_ = 0;
};

// ---------------- EMC2103 风扇控制器（SMSC，固定从地址 0x2E） ----------------
// 转速：0x4E 高字节 / 0x4F 低字节，13 位左对齐（低 3 位恒为 0）。
//   COUNT = (hi << 5) | (lo >> 3)，RPM = 3932160 / (COUNT × m)，
//   常量 3932160 = 60 × 32768 / 5（2 极扇、5 边沿、32.768 kHz tach 时钟）。
//   m 由 RANGE[1:0] 决定，本实现取默认量程 1000 RPM 的 m = 2。
//   COUNT == 0x1FE0 是"风扇停转/未接"哨兵值（寄存器默认值），不是转速。
// 温度：每通道 16 位有符号、0.125 ℃ 分辨率，C = (int16)((hi << 8) | lo) / 256。
//   0x8000 表示二极管故障，不能当作 -128 ℃ 上报。
class Emc2103Driver final : public ChipDriver {
public:
    explicit Emc2103Driver(I2cBus& bus) : bus_(bus) {}
    const char* name() const override { return "emc2103"; }
    std::vector<ChipFeature> features() const override {
        return {{"rpm", "RPM"}, {"temp", "C"}, {"temp_external", "C"}};
    }
    std::optional<double> read(const std::string& feature) override {
        if (same_feature(feature, "rpm")) {
            const auto high = bus_.read_byte(kTachHigh);
            if (!high) return std::nullopt;
            const auto low = bus_.read_byte(kTachLow);
            if (!low) return std::nullopt;
            const auto count = static_cast<std::uint16_t>((static_cast<std::uint16_t>(*high) << 5) |
                                                           (static_cast<std::uint16_t>(*low) >> 3));
            // 哨兵：寄存器默认值 FFh/F8h，且高 5 位全 1 表示风扇停转或未接。
            // 按掩码判断而不做等值比较，这样低 3 位即便不是 0 也不会误判为转速。
            if ((count & 0x1FE0) == 0x1FE0 || count == 0) {
                return std::nullopt;
            }
            return kTachConstant * kRangeMultiplier / static_cast<double>(count);
        }
        if (same_feature(feature, "temp")) {
            return read_temperature(kInternalHigh, kInternalLow);
        }
        if (same_feature(feature, "temp_external")) {
            return read_temperature(kExternalHigh, kExternalLow);
        }
        return std::nullopt;
    }

private:
    std::optional<double> read_temperature(std::uint8_t high_command, std::uint8_t low_command) const {
        const auto high = bus_.read_byte(high_command);
        if (!high) return std::nullopt;
        const auto low = bus_.read_byte(low_command);
        if (!low) return std::nullopt;
        const auto raw = from_register_bytes(*high, *low);
        if (raw == kDiodeFault) {
            return std::nullopt;
        }
        return static_cast<double>(to_signed(raw)) / 256.0;
    }
    static constexpr std::uint8_t kInternalHigh = 0x00;
    static constexpr std::uint8_t kInternalLow = 0x01;
    static constexpr std::uint8_t kExternalHigh = 0x02;
    static constexpr std::uint8_t kExternalLow = 0x03;
    static constexpr std::uint8_t kTachHigh = 0x4E;
    static constexpr std::uint8_t kTachLow = 0x4F;
    static constexpr std::uint16_t kDiodeFault = 0x8000;
    static constexpr double kTachConstant = 3932160.0;
    static constexpr double kRangeMultiplier = 2.0;
    I2cBus& bus_;
};

// ---------------- INA219 / INA226 电流功率监视器 ----------------
// 两者寄存器指针相同，但 LSB 与位域不同：
//   INA219：分流 10 uV（有符号），母线 4 mV 且数据在 bit[15:3]（bit1=CNVR，bit0=OVF），
//           校准系数 0.04096，功率 LSB = 20 × Current_LSB。
//   INA226：分流 2.5 uV（有符号），母线 1.25 mV 且为 15 位无需移位，
//           校准系数 0.00512，功率 LSB = 25 × Current_LSB。
// 电流与功率寄存器在写入校准寄存器之前恒为 0，因此构造时即写默认校准值。
class InaDriver final : public ChipDriver {
public:
    InaDriver(I2cBus& bus, bool is_226, double shunt_ohms, double max_current) : bus_(bus), is_226_(is_226) {
        const double default_calibration = is_226_ ? 2048.0 : 4096.0;
        if (shunt_ohms > 0 && max_current > 0) {
            const double current_lsb = max_current / 32768.0;
            const double factor = is_226_ ? kIna226CalFactor : kIna219CalFactor;
            const double computed = factor / (current_lsb * shunt_ohms);
            if (computed >= 1 && computed <= 65535) {
                calibration_ = static_cast<std::uint16_t>(computed);
                // 由实际写下的校准值反推 Current_LSB，保证电流/功率换算与硬件一致。
                current_lsb_ = factor / (static_cast<double>(calibration_) * shunt_ohms);
            }
        }
        if (calibration_ == 0) {
            calibration_ = static_cast<std::uint16_t>(default_calibration);
            if (shunt_ohms > 0) {
                // 默认校准下电流寄存器数值等于分流电压寄存器数值。
                current_lsb_ = (is_226_ ? kIna226ShuntLsbVolts : kIna219ShuntLsbVolts) / shunt_ohms;
            } else {
                // 未给分流电阻时无法换算成安培/瓦，但保留默认 LSB 使 Raw 值可用。
                current_lsb_ = is_226_ ? kIna226ShuntLsbVolts : kIna219ShuntLsbVolts;
            }
        }
        if (!is_226_) {
            // INA219 校准寄存器 FS0 恒为 0，不可写入 1。
            calibration_ &= 0xFFFE;
        }
        if (!bus_.write_word(kCalibration, calibration_)) {
            throw std::runtime_error("program i2c calibration failed");
        }
    }
    const char* name() const override { return is_226_ ? "ina226" : "ina219"; }
    std::vector<ChipFeature> features() const override {
        return {{"bus", "V"}, {"shunt", "V"}, {"current", "A"}, {"power", "W"}};
    }
    std::optional<double> read(const std::string& feature) override {
        if (same_feature(feature, "shunt")) {
            const auto raw = bus_.read_word(kShuntVoltage);
            if (!raw) return std::nullopt;
            return static_cast<double>(to_signed(*raw)) * shunt_lsb();
        }
        if (same_feature(feature, "bus")) {
            const auto raw = bus_.read_word(kBusVoltage);
            if (!raw) return std::nullopt;
            // 右移丢弃 CNVR/OVF；INA219 还需丢弃未定义的 bit2。
            return static_cast<double>(*raw >> bus_shift()) * bus_lsb();
        }
        if (same_feature(feature, "current")) {
            if (!(current_lsb_ > 0)) return std::nullopt;
            const auto raw = bus_.read_word(kCurrent);
            if (!raw) return std::nullopt;
            return static_cast<double>(to_signed(*raw)) * current_lsb_;
        }
        if (same_feature(feature, "power")) {
            if (!(current_lsb_ > 0)) return std::nullopt;
            const auto raw = bus_.read_word(kPower);
            if (!raw) return std::nullopt;
            return static_cast<double>(*raw) * (is_226_ ? kIna226PowerFactor : kIna219PowerFactor) * current_lsb_;
        }
        return std::nullopt;
    }

private:
    double shunt_lsb() const { return is_226_ ? kIna226ShuntLsbVolts : kIna219ShuntLsbVolts; }
    double bus_lsb() const { return is_226_ ? kIna226BusLsbVolts : kIna219BusLsbVolts; }
    unsigned bus_shift() const { return is_226_ ? 0u : 3u; }
    static constexpr std::uint8_t kShuntVoltage = 0x01;
    static constexpr std::uint8_t kBusVoltage = 0x02;
    static constexpr std::uint8_t kPower = 0x03;
    static constexpr std::uint8_t kCurrent = 0x04;
    static constexpr std::uint8_t kCalibration = 0x05;
    static constexpr double kIna219CalFactor = 0.04096;
    static constexpr double kIna219ShuntLsbVolts = 1e-5;
    static constexpr double kIna219BusLsbVolts = 0.004;
    static constexpr double kIna219PowerFactor = 20;
    static constexpr double kIna226CalFactor = 0.00512;
    static constexpr double kIna226ShuntLsbVolts = 2.5e-6;
    static constexpr double kIna226BusLsbVolts = 0.00125;
    static constexpr double kIna226PowerFactor = 25;
    I2cBus& bus_;
    bool is_226_ = false;
    std::uint16_t calibration_ = 0;
    double current_lsb_ = 0;
};
}

std::vector<std::string> chip_names() {
    return {"lm75", "lm75b", "emc2103", "adm1275", "ina219", "ina226"};
}

std::unique_ptr<ChipDriver> make_chip(const std::string& name, I2cBus& bus, const std::string& feature) {
    std::unique_ptr<ChipDriver> driver;
    if (name == "lm75") {
        driver = std::make_unique<Lm75Driver>(bus, false);
    } else if (name == "lm75b") {
        driver = std::make_unique<Lm75Driver>(bus, true);
    } else if (name == "emc2103") {
        driver = std::make_unique<Emc2103Driver>(bus);
    } else if (name == "adm1275") {
        driver = std::make_unique<Adm1275Driver>(bus);
    } else if (name == "ina219") {
        driver = std::make_unique<InaDriver>(bus, false, 0, 0);
    } else if (name == "ina226") {
        driver = std::make_unique<InaDriver>(bus, true, 0, 0);
    } else {
        throw std::invalid_argument("unknown i2c chip: " + name);
    }
    // 型号不提供的测量量在构造时就拒绝，而不是运行期静默返回空。
    if (!feature_supported(feature, driver->features())) {
        throw std::invalid_argument("chip " + name + " has no feature: " + feature);
    }
    return driver;
}
}
