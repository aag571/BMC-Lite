#pragma once
#include "bmc/linux_io.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bmc {
// 一个 I2C 目标设备的 SMBus 访问封装。所有系统调用都经注入的 LinuxIo，
// 因此寄存器解码逻辑可以用 FakeLinuxIo 覆盖，无需真实 /dev/i2c-* 适配器。
class I2cBus {
public:
    I2cBus(const std::string& device, std::uint8_t address, LinuxIo& io);
    I2cBus(const I2cBus&) = delete;
    I2cBus& operator=(const I2cBus&) = delete;
    ~I2cBus();

    std::uint8_t address() const { return address_; }
    // 单字节寄存器读：SMBus Read Byte Data。
    std::optional<std::uint8_t> read_byte(std::uint8_t command);
    // 16 位寄存器读：SMBus Read Word Data。总线按小端返回，本方法负责还原成主机字节序的数值。
    std::optional<std::uint16_t> read_word(std::uint8_t command);
    // 连续块读：先写寄存器指针，再按 SMBus Read I2C Block 读 count 字节（最多 32）。
    std::optional<std::vector<std::uint8_t>> read_block(std::uint8_t command, std::uint8_t count);
    // 寄存器写（部分芯片需要先写配置、校准或页选择）。
    bool write_byte(std::uint8_t command, std::uint8_t value);
    bool write_word(std::uint8_t command, std::uint16_t value);

private:
    int descriptor_ = -1;
    std::uint8_t address_ = 0;
    LinuxIo* io_ = nullptr;
};

// 一块芯片可能提供多个测量量（温度/转速/电压/电流/功率）。
struct ChipFeature {
    std::string name;
    std::string unit;
};

class ChipDriver {
public:
    virtual ~ChipDriver() = default;
    virtual const char* name() const = 0;
    // 该芯片型号提供的全部测量量，用于报错信息与文档。
    virtual std::vector<ChipFeature> features() const = 0;
    // 读取某个测量量并换算为工程值；不支持的名字返回 std::nullopt。
    virtual std::optional<double> read(const std::string& feature) = 0;
};

// 按型号名构造驱动。未知型号抛出 std::invalid_argument。
// feature 为空时使用该型号的默认测量量。
std::unique_ptr<ChipDriver> make_chip(const std::string& name, I2cBus& bus, const std::string& feature);
// 驱动注册表里的型号名，供配置校验提示。
std::vector<std::string> chip_names();
// I2cBus::read_word 已由内核完成字节序还原（返回主机字节序）。
// 但芯片寄存器内部的位域多是"高字节在前"，因此需要按寄存器语义拼装：
// 下面的 C 是寄存器的高位字节，B 是低位字节，结果为 (C << 8) | B。
std::uint16_t from_register_bytes(std::uint8_t high, std::uint8_t low);
// 把 16 位二进制补码解释为有符号整数。
std::int16_t to_signed(std::uint16_t value);
}
