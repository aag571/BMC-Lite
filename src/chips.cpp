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

// chips.cpp —— I2C/SMBus 总线封装与专用芯片驱动（接口声明在 bmc/chip.hpp）。
// 本文件只负责寄存器语义与换算；系统调用全部经注入的 LinuxIo，使解码逻辑可在 FakeLinuxIo 下单测。
// 凡未与发布版数据手册逐项核对的常量都在此标注“未核实”，依据见 docs/芯片驱动与标定.md。
namespace bmc {
namespace {
// 把当前 errno 转成异常：必须在失败的系统调用之后立刻调用，中间不能插入可能改写 errno 的操作。
[[noreturn]] void system_failure(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
// SMBus 传输的公共部分：填好 i2c_smbus_ioctl_data 后发出 ioctl。
// size 决定事务形态：I2C_SMBUS_BYTE_DATA 读写单字节寄存器，I2C_SMBUS_WORD_DATA 读写 16 位寄存器，
// I2C_SMBUS_I2C_BLOCK_DATA 为块传输；operation 取 I2C_SMBUS_READ 或 I2C_SMBUS_WRITE。
// 返回值只表示 ioctl 成功与否，取值由调用方从 data 读取；失败原因留在 errno。
bool smbus(int descriptor, LinuxIo& io, char operation, std::uint8_t command, int size, i2c_smbus_data& data) {
    i2c_smbus_ioctl_data request {};
    request.read_write = operation;
    request.command = command;
    request.size = size;
    request.data = &data;
    return io.ioctl(descriptor, I2C_SMBUS, &request) >= 0;
}
// 测量量名必须与驱动声明的名字逐字相等（来自配置里的 @feature 后缀），不做别名或大小写匹配。
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

// 芯片寄存器内部常见“高字节在前”的位域，用两次单字节读拼回 16 位：结果 = (high << 8) | low。
// 与 I2cBus::read_word 不同：后者由内核完成 SMBus 传输的字节序还原，返回的已是主机字节序数值。
std::uint16_t from_register_bytes(std::uint8_t high, std::uint8_t low) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8) | low);
}

// 把 16 位二进制补码理解为有符号整数：最高位是符号位，为 1 时按负值解释。
std::int16_t to_signed(std::uint16_t value) {
    return static_cast<std::int16_t>(value);
}

// 打开 /dev/i2c-* 并用 I2C_SLAVE 选中从地址；该 ioctl 的第三个参数是整数地址，
// 按 ioctl 约定先转 uintptr_t 再伪装成 void*，不能直接传整数。
// 随后查询适配器能力，要求同时具备 SMBus BYTE_DATA 与 WORD_DATA：本文件的驱动只用这两类事务，
// 能力缺失属于硬件/配置选择错误，在构造期报错比让每次采样静默失败更容易定位。
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

// 关闭描述符；io_ 判空是防御性的，它在构造列表里必然已被赋值。
I2cBus::~I2cBus() {
    if (descriptor_ >= 0 && io_ != nullptr) {
        io_->close(descriptor_);
    }
}

// SMBus Read Byte Data：命令字节即寄存器指针，没有数据字节。
// 传输失败返回 nullopt；只有 open/能力检查这类构造期错误才抛异常。
std::optional<std::uint8_t> I2cBus::read_byte(std::uint8_t command) {
    i2c_smbus_data data {};
    if (!smbus(descriptor_, *io_, I2C_SMBUS_READ, command, I2C_SMBUS_BYTE_DATA, data)) {
        return std::nullopt;
    }
    return data.byte;
}

// SMBus Read Word Data：一次事务读回 16 位寄存器，内核已按 SMBus 协议完成字节序还原，
// 因此返回值就是主机字节序数值，调用方不要再手工调换高低字节。
std::optional<std::uint16_t> I2cBus::read_word(std::uint8_t command) {
    i2c_smbus_data data {};
    if (!smbus(descriptor_, *io_, I2C_SMBUS_READ, command, I2C_SMBUS_WORD_DATA, data)) {
        return std::nullopt;
    }
    // 内核已完成 SMBus 传输的字节序还原，data.word 即主机字节序数值。
    return static_cast<std::uint16_t>(data.word);
}

// SMBus Read I2C Block Data：先写寄存器指针，再连读最多 32 字节。
// count 上限 32 源自 i2c_smbus_data.block 的容量（1 字节长度 + 32 字节载荷），越界在发出 ioctl 前就拒绝。
// 返回长度取适配器回报的 data.block[0] 与请求 count 的较小值，防止适配器多报导致读越界。
// 目前没有驱动调用本方法，构造期的能力检查也未包含 I2C_FUNC_SMBUS_I2C_BLOCK_DATA。
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

// SMBus Write Byte Data：写单字节寄存器（配置、页选择等）。返回是否成功。
// 目前本文件的驱动都不调用它。
bool I2cBus::write_byte(std::uint8_t command, std::uint8_t value) {
    i2c_smbus_data data {};
    data.byte = value;
    return smbus(descriptor_, *io_, I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA, data);
}

// SMBus Write Word Data：写 16 位寄存器（如 INA 校准值）。
// value 按主机字节序填入 data.word，上线的字节序由内核按 SMBus 协议负责。
bool I2cBus::write_word(std::uint8_t command, std::uint16_t value) {
    i2c_smbus_data data {};
    data.word = value;
    return smbus(descriptor_, *io_, I2C_SMBUS_WRITE, command, I2C_SMBUS_WORD_DATA, data);
}

namespace {
// ---------------- LM75 / LM75A / LM75B 温度传感器 ----------------
// 温度寄存器 0x00，16 位、高字节在前的二进制补码。LM75A/B 低字节的 bit7..bit5 是 0.125 ℃ 位，
// 因此 11 位分辨率下 LSB = 0.125 ℃。原始 LM75 只有 0.5 ℃ 分辨率，低字节为 0。
// 家族内分辨率并不统一：构造参数 high_resolution 选 false 走 9 位、选 true 走 11 位，
// 型号名随之不同；哪个具体型号属于哪种分辨率未核实（见 docs/芯片驱动与标定.md）。
// 读取只用 read_word 一次完整事务，不用两次单字节读，以免触发 LM75A 单字节读把 SDA 拉低的已知陷阱。
class Lm75Driver final : public ChipDriver {
public:
    Lm75Driver(I2cBus& bus, bool high_resolution) : bus_(bus), high_resolution_(high_resolution) {}
    const char* name() const override { return high_resolution_ ? "lm75b" : "lm75"; }
    std::vector<ChipFeature> features() const override { return {{"temp", "C"}}; }
    // 两种分辨率的差别只在右移位数与 LSB；读失败（从机无应答、超时）与分辨率无关，
    // 一律返回 nullopt，由上层记为无读数而不是故障。
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
// 当前只提供 vin/vout/iout；“31 条命令、无 READ_PIN/温度命令”未核实到 Rev.E 具体表项。
// READ_VIN 0x88 / READ_VOUT 0x8B / READ_IOUT 0x8C，数据右对齐在 bit[11:0]，无符号。
// DIRECT 格式下同一段码值在不同量程/分流电阻下对应不同工程值，所以驱动不缓存原始码，
// 电压与电流各有一处换算实现；配置侧的量程必须与实际硬件一致，否则读数整体偏移。
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
    // 原始字右对齐在 bit[11:0]，其余位不是数据，必须先掩掉再当 12 位码使用。
    // 未核实：LSB 与 PMON_CONFIG VRANGE=1（0–20 V 默认量程）的对应关系尚未对照 Rev.E 表项。
    // 数据手册的 LSB 法比舍入后的 m/b/R 系数更精确：V = LSB × (code + 0.5)。
    // +0.5 是半码偏移：把码代表的量化区间中心还原为实际电压，而不是区间下沿。
    std::optional<double> code_to_volts(std::uint8_t command) const {
        const auto raw = bus_.read_word(command);
        if (!raw) {
            return std::nullopt;
        }
        const double code = static_cast<double>(*raw & 0x0FFF);
        return voltage_lsb_ * (code + 0.5);
    }
    // 12 位码用于双向电流：零点在 2048，减去 2048 得到有符号电流码。
    // I = 12.4 uV × (code − 2048) / R_SENSE；未配置分流电阻时无法给出安培值。
    // 分流电阻以毫欧配置，1e-3 在这里换算成欧姆。
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
    // 未核实：5.208 mV 与该量程的对应关系尚未对照 Rev.E 具体表项，改量程必须同步改此值。
    double voltage_lsb_ = 5.208e-3;
    // 分流电阻（毫欧）。0 表示未配置，此时 iout 无法换算成安培。
    double shunt_milliohms_ = 0;
};

// ---------------- EMC2103 风扇控制器（SMSC，固定从地址 0x2E） ----------------
// 未核实：以下换算常量、0x8000 故障码、内部分辨率与默认 RANGE 尚未对照 DS20006705 发布版。
// 本实现及 Fake 测试只能证明当前假设的一致性，不能代替真实芯片数据手册核验。
// 转速：0x4E 高字节 / 0x4F 低字节，13 位左对齐（低 3 位恒为 0）。
//   COUNT = (hi << 5) | (lo >> 3)，RPM = 3932160 / (COUNT × m)，
//   3932160 是当前实现的候选常量，时钟/边沿推导未核实，不声称与发布版一致。
//   m 由 RANGE[1:0] 决定，本实现取默认量程 1000 RPM 的 m = 2。
//   COUNT == 0x1FE0 是"风扇停转/未接"哨兵值（寄存器默认值），不是转速。
// 温度：每通道 16 位有符号、0.125 ℃ 分辨率，C = (int16)((hi << 8) | lo) / 256。
//   0x8000 表示二极管故障，不能当作 -128 ℃ 上报。
// 从地址固定 0x2E，配置里的 device,address 必须与之一致。本驱动只读不写，
// 量程沿用芯片上电默认值，因此外部若改过 Fan Configuration 1，转速换算会整体偏移。
class Emc2103Driver final : public ChipDriver {
public:
    explicit Emc2103Driver(I2cBus& bus) : bus_(bus) {}
    const char* name() const override { return "emc2103"; }
    std::vector<ChipFeature> features() const override {
        return {{"rpm", "RPM"}, {"temp", "C"}, {"temp_external", "C"}};
    }
    // 每次调用只读一种测量量。转速的高低字节分两次字节读，手册未记录影子/锁存寄存器，
    // 因此两次读之间数值可能变化，本实现不声称原子性（见 docs/芯片驱动与标定.md 未核实项）。
    std::optional<double> read(const std::string& feature) override {
        if (same_feature(feature, "rpm")) {
            const auto high = bus_.read_byte(kTachHigh);
            if (!high) return std::nullopt;
            const auto low = bus_.read_byte(kTachLow);
            if (!low) return std::nullopt;
            // 拼装 13 位左对齐计数：高字节整体落在 bit12..bit5，低字节只有 bit7..bit3 有效。
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
    // 16 位有符号、0.125 ℃ 分辨率：先按“高字节在前”拼装，再按二进制补码解释并除以 256。
    // 0x8000 是二极管故障码（未核实），必须当无读数返回，不能按 -128 ℃ 上报。
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
    // 未核实到发布版；保留当前兼容行为，避免没有依据地改动寄存器换算。
    static constexpr std::uint16_t kDiodeFault = 0x8000;
    // 未核实：3932160 是候选常量，时钟/边沿推导未记录；m 取默认 1000 RPM 量程的 2。
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
// 校准寄存器必须在读电流/功率之前写好：未校准前这两个寄存器读回的是 0，而不是真实值。
// 参数齐全时按数据手册公式算校准值，再由写下的校准值反推 current_lsb_，使换算与硬件一致；
// 参数缺失或算得的校准值超出 1..65535 时退化为默认校准（INA219 4096 / INA226 2048）。
class InaDriver final : public ChipDriver {
public:
    // 构造即写校准寄存器，失败直接抛异常：宁可构造失败，也不要让后续电流/功率静默读成 0。
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
    // 四个测量量共用同一批寄存器指针，差别只在 LSB、位宽与是否有符号；
    // 缺少换算依据（如 current_lsb_ 无效）或读失败都返回 nullopt。
    std::optional<double> read(const std::string& feature) override {
        if (same_feature(feature, "shunt")) {
            const auto raw = bus_.read_word(kShuntVoltage);
            if (!raw) return std::nullopt;
            // 分流电压 16 位有符号，LSB 10 uV（INA219）/ 2.5 uV（INA226）。
            return static_cast<double>(to_signed(*raw)) * shunt_lsb();
        }
        if (same_feature(feature, "bus")) {
            const auto raw = bus_.read_word(kBusVoltage);
            if (!raw) return std::nullopt;
            // 母线电压只读有效位：INA226 为 15 位不需移位，INA219 的数据在 bit[15:3]。
            // 右移丢弃 CNVR/OVF；INA219 还需丢弃未定义的 bit2。
            return static_cast<double>(*raw >> bus_shift()) * bus_lsb();
        }
        if (same_feature(feature, "current")) {
            if (!(current_lsb_ > 0)) return std::nullopt;
            const auto raw = bus_.read_word(kCurrent);
            if (!raw) return std::nullopt;
            // 电流寄存器有符号，LSB 为 current_lsb_（由校准值反推），单位为安培。
            return static_cast<double>(to_signed(*raw)) * current_lsb_;
        }
        if (same_feature(feature, "power")) {
            if (!(current_lsb_ > 0)) return std::nullopt;
            const auto raw = bus_.read_word(kPower);
            if (!raw) return std::nullopt;
            // 功率寄存器无符号，LSB = 固定系数 × Current_LSB（INA219 系数 20，INA226 系数 25）。
            return static_cast<double>(*raw) * (is_226_ ? kIna226PowerFactor : kIna219PowerFactor) * current_lsb_;
        }
        return std::nullopt;
    }

private:
    // 三个换算参数按型号取值，集中在这里以免 read() 里散落 is_226_ 分支。
    double shunt_lsb() const { return is_226_ ? kIna226ShuntLsbVolts : kIna219ShuntLsbVolts; }
    double bus_lsb() const { return is_226_ ? kIna226BusLsbVolts : kIna219BusLsbVolts; }
    unsigned bus_shift() const { return is_226_ ? 0u : 3u; }
    static constexpr std::uint8_t kShuntVoltage = 0x01;
    static constexpr std::uint8_t kBusVoltage = 0x02;
    static constexpr std::uint8_t kPower = 0x03;
    static constexpr std::uint8_t kCurrent = 0x04;
    static constexpr std::uint8_t kCalibration = 0x05;
    // 以下 LSB、校准系数与功率系数均未核实（SBOS448G/SBOS547C 的具体章节未记录，见 docs/芯片驱动与标定.md）。
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

// 驱动注册表的型号名，供配置校验与错误提示；新增型号需同时更新这里与 make_chip。
std::vector<std::string> chip_names() {
    return {"lm75", "lm75b", "emc2103", "adm1275", "ina219", "ina226"};
}

// 按配置里的 i2c:<chip> 构造驱动；未知型号抛 std::invalid_argument。
// 返回的驱动只持有 I2cBus 的引用，bus 的生命周期必须覆盖驱动（ChipReader 靠成员声明顺序保证）。
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
