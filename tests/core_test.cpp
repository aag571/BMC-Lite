#include "bmc/core.hpp"
#include "bmc/action.hpp"
#include "bmc/chip.hpp"
#include "bmc/cli.hpp"
#include "bmc/http.hpp"
#include "bmc/service.hpp"
#include "bmc/socket_io.hpp"
#include "service_vectors.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <gtest/gtest.h>
#include <limits>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <fstream>
#include <fcntl.h>
#include <future>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

// 本文件覆盖 bmc-lite 中除网络与控制面之外的核心逻辑：
//   - Engine / Config / Reader / Pwm：阈值判定、去抖与迟滞、配置解析、读取器与 PWM 写入；
//   - Calibration：线性项、分段插值与区间外钳制；
//   - Chips：lm75 / adm1275 / ina219 / ina226 / emc2103 的寄存器解码与换算；
//   - Device / EventBus / Worker / Rules / Recovery：设备生命周期、事件分发、任务队列与恢复策略；
//   - Sel / Logger：持久化格式、批量落盘、压实、撕裂尾部修复与写失败降级；
//   - Cli / SocketIo / HttpParser / RateLimiter / ReadOnlyService：命令行与 TCP 外围组件。
// 除少数直接验证 Posix 直通实现的用例外，硬件访问全部由伪造的 LinuxIo / SocketIo 承接。
namespace {
bmc::Config policy() {
    bmc::Config config;
    config.id = "cpu";
    config.backend = "mock";
    config.path = "40,75,95,err";
    return config;
}
std::filesystem::path temporary(const std::string& suffix) {
    return std::filesystem::temp_directory_path() / ("bmc-test-" + std::to_string(::getpid()) + suffix);
}
// 脚本化的 LinuxIo：让 I2C / GPIO / PWM 路径不需要真实 /dev/i2c-* 与 /dev/gpiochip* 就能被单元测试覆盖。
// 可脚本化的虚调用与配置方式：
//   - open()：按路径分配并复用描述符；"/dev/fake-open-failure" 稳定返回 ENOENT。
//   - ioctl()：应答存放在 Descriptor::replies 中。SMBus 字节读以命令码为键，字/块读统一以
//     I2C_SMBUS 为键（寄存器号在 transaction->command 中）；序列多于一个元素时逐次消费，
//     只剩一个元素时反复返回。I2C_FUNCS 的能力位来自 i2c_functions，用例可直接改写。
//   - GPIO 行请求会真的 ::open("/dev/null", O_RDONLY | O_CLOEXEC)，因此 CLOEXEC 与析构关闭
//     由真实描述符验证，只有行值来自脚本化 ioctl。
//   - write()：内容记入 Descriptor::writes；short_write 让下一次写返回 count-1，
//     用来验证调用方确实会检测短写。
// 明确不模拟：真实芯片时序、SMBus 错误状态、内核返回的零散字节数与并发访问。
class FakeLinuxIo final : public bmc::LinuxIo {
public:
    struct Descriptor {
        std::map<unsigned long, std::vector<std::uint8_t>> replies;
        // SMBus 写：命令码 -> 写下的值，供断言芯片初始化（例如写入校准寄存器）。
        std::map<std::uint8_t, std::uint16_t> registers;
        std::vector<std::string> writes;
        bool short_write = false;
    };
    int open(const std::string& path, int) override {
        opened.push_back(path);
        if (path == "/dev/fake-open-failure") {
            errno = ENOENT;
            return -1;
        }
        const auto existing = path_descriptors.find(path);
        if (existing != path_descriptors.end()) {
            return existing->second;
        }
        const int descriptor = next_descriptor++;
        path_descriptors[path] = descriptor;
        descriptors[descriptor];
        return descriptor;
    }
    int ioctl(int descriptor, unsigned long request, void* argument) override {
        ++ioctls;
        const auto found = descriptors.find(descriptor);
        if (found == descriptors.end()) {
            errno = ENODEV;
            return -1;
        }
        auto& entry = found->second;
        // SMBus 请求码在本文件里被当作"任意寄存器读"的标记：具体寄存器号在 transaction->command。
        // 因此读应答的键是：字节读用命令码（一个命令一个序列），字/块读用 I2C_SMBUS。
        const bool is_smbus = request == I2C_SMBUS;
        auto* transaction = is_smbus ? static_cast<i2c_smbus_ioctl_data*>(argument) : nullptr;
        const bool is_smbus_write = is_smbus && transaction->read_write == I2C_SMBUS_WRITE;
        const unsigned long reply_key = !is_smbus ? request
            : (transaction->size == I2C_SMBUS_BYTE_DATA ? transaction->command : request);
        const auto reply = entry.replies.find(reply_key);
        // 不需要回复脚本的四类：纯成功型（I2C_SLAVE、GPIO_V2_GET_LINE_IOCTL）、
        // 内容由测试成员提供（I2C_FUNCS、GPIO_GET_CHIPINFO_IOCTL）、线值读（缺省为 0）、写事务。
        const bool needs_reply = request != I2C_SLAVE && request != I2C_FUNCS &&
            request != GPIO_GET_CHIPINFO_IOCTL && request != GPIO_V2_GET_LINE_IOCTL &&
            request != GPIO_V2_LINE_GET_VALUES_IOCTL && !is_smbus_write;
        if (needs_reply && reply == entry.replies.end()) {
            errno = ENOTTY;
            return -1;
        }
        // 取本次应答：序列长度大于 1 时消费队首，只剩一个元素时反复返回它。
        auto take_byte = [&entry](unsigned long key) -> std::uint8_t {
            auto& bytes = entry.replies[key];
            const std::uint8_t value = bytes.front();
            if (bytes.size() > 1) {
                bytes.erase(bytes.begin());
            }
            return value;
        };
        // 回填假数据，模拟内核对调用方传入结构体的写入。
        if (request == I2C_SLAVE) {
            ++selected_slaves;
        } else if (request == I2C_FUNCS) {
            *static_cast<unsigned long*>(argument) = i2c_functions;
        } else if (is_smbus) {
            auto* data = static_cast<i2c_smbus_data*>(transaction->data);
            if (is_smbus_write) {
                // 写事务没有回复脚本，登记即表示成功；按尺寸记录写入内容。
                ++smbus_writes;
                if (transaction->size == I2C_SMBUS_BYTE_DATA) {
                    entry.registers[transaction->command] = data->byte;
                } else {
                    entry.registers[transaction->command] = static_cast<std::uint16_t>(data->word);
                }
            } else if (transaction->size == I2C_SMBUS_BYTE_DATA) {
                data->byte = take_byte(transaction->command);
            } else if (transaction->size == I2C_SMBUS_I2C_BLOCK_DATA) {
                auto& bytes = entry.replies[request];
                const std::size_t count = std::min<std::size_t>(bytes.size(), 32);
                data->block[0] = static_cast<std::uint8_t>(count);
                std::memcpy(data->block + 1, bytes.data(), count);
            } else {
                // 读应答按 ioctl 请求码（I2C_SMBUS）登记，因为该请求表示"任意寄存器读"，
                // 寄存器号只出现在 transaction->command 中。此处不能再使用 reply 迭代器：
                // take_byte/operator[] 会修改 entry.replies 使其失效。
                std::uint16_t word = 0;
                const auto& bytes = entry.replies[request];
                if (!bytes.empty()) {
                    std::memcpy(&word, bytes.data(), sizeof(word));
                }
                data->word = word;
            }
        } else if (request == GPIO_GET_CHIPINFO_IOCTL) {
            static_cast<gpiochip_info*>(argument)->lines = gpio_lines;
        } else if (request == GPIO_V2_GET_LINE_IOCTL) {
            auto* line_request = static_cast<gpio_v2_line_request*>(argument);
            line_requests.push_back(line_request->config.flags);
            line_descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            descriptors[line_descriptor].replies[GPIO_V2_LINE_GET_VALUES_IOCTL] = {1};
            line_request->fd = line_descriptor;
        } else if (request == GPIO_V2_LINE_GET_VALUES_IOCTL) {
            const auto bits = entry.replies.find(request);
            static_cast<gpio_v2_line_values*>(argument)->bits =
                bits == entry.replies.end() || bits->second.empty() ? 0 : bits->second.at(0);
        }
        return 0;
    }
    ssize_t read(int, void*, std::size_t) override {
        errno = EIO;
        return -1;
    }
    ssize_t write(int descriptor, const void* buffer, std::size_t count) override {
        const auto found = descriptors.find(descriptor);
        if (found == descriptors.end()) {
            errno = EBADF;
            return -1;
        }
        found->second.writes.emplace_back(static_cast<const char*>(buffer), count);
        if (found->second.short_write) {
            return static_cast<ssize_t>(count) - 1;
        }
        return static_cast<ssize_t>(count);
    }
    unsigned long i2c_functions = I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_READ_WORD_DATA;
    std::uint32_t gpio_lines = 32;
    std::map<std::string, int> path_descriptors;
    std::map<int, Descriptor> descriptors;
    std::vector<std::uint64_t> line_requests;
    std::vector<std::string> opened;
    int ioctls = 0;
    int selected_slaves = 0;
    int smbus_writes = 0;
    int next_descriptor = 101;
    int line_descriptor = -1;
};
// ============ Engine：去抖、迟滞与读取失败 ============
TEST(Engine, DebouncesCriticalAndUsesInclusiveBoundary) {
    bmc::Engine engine(policy());
    EXPECT_FALSE(engine.update(90));
    EXPECT_FALSE(engine.update(90));
    const auto event = engine.update(90);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->after, bmc::State::critical);
    EXPECT_EQ(event->before, bmc::State::normal);
    EXPECT_EQ(event->sequence, 1u);
    EXPECT_FALSE(engine.update(100));
}
TEST(Engine, HysteresisRequiresCrossingRecoveryBoundary) {
    auto config = policy();
    config.debounce = 1;
    bmc::Engine engine(config);
    ASSERT_TRUE(engine.update(90));
    EXPECT_FALSE(engine.update(87));
    ASSERT_TRUE(engine.update(86));
    EXPECT_EQ(engine.state(), bmc::State::warning);
    EXPECT_FALSE(engine.update(67));
    ASSERT_TRUE(engine.update(66));
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
TEST(Engine, AlternatingValuesResetDebounce) {
    bmc::Engine engine(policy());
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        EXPECT_FALSE(engine.update(95));
        EXPECT_FALSE(engine.update(40));
    }
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
TEST(Engine, InvalidSamplesBecomeUnavailableAndRecover) {
    bmc::Engine engine(policy());
    EXPECT_FALSE(engine.update(std::nullopt));
    EXPECT_FALSE(engine.update(std::numeric_limits<double>::infinity()));
    const auto event = engine.update(std::numeric_limits<double>::quiet_NaN());
    ASSERT_TRUE(event);
    EXPECT_EQ(event->after, bmc::State::unavailable);
    EXPECT_FALSE(event->value);
    EXPECT_FALSE(engine.update(40));
    EXPECT_FALSE(engine.update(40));
    ASSERT_TRUE(engine.update(40));
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
// 读取失败会清空已累计的去抖计数，恢复后必须重新数满 debounce 次才会翻转。
TEST(Engine, FailedReadingBreaksPendingTransition) {
    bmc::Engine engine(policy());
    EXPECT_FALSE(engine.update(95));
    EXPECT_FALSE(engine.update(95));
    EXPECT_FALSE(engine.update(std::nullopt));
    EXPECT_FALSE(engine.update(95));
    EXPECT_FALSE(engine.update(95));
    EXPECT_TRUE(engine.update(95));
}
TEST(Engine, LowDirectionAndRecovery) {
    auto config = policy();
    config.high = false;
    config.warning = 1500;
    config.critical = 500;
    config.hysteresis = 100;
    config.debounce = 1;
    bmc::Engine engine(config);
    ASSERT_TRUE(engine.update(500));
    EXPECT_EQ(engine.state(), bmc::State::critical);
    EXPECT_FALSE(engine.update(600));
    ASSERT_TRUE(engine.update(601));
    EXPECT_EQ(engine.state(), bmc::State::warning);
    EXPECT_FALSE(engine.update(1600));
    ASSERT_TRUE(engine.update(1601));
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
// ============ Config / Reader / Logger / Worker / Pwm：基础组件与校验 ============
TEST(Config, RejectsBrokenThresholdsAndWindows) {
    auto config = policy();
    config.critical = config.warning;
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.debounce = 0;
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.scale = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
}
TEST(Reader, MockCyclesAndScales) {
    auto config = policy();
    config.scale = 2;
    auto reader = bmc::make_reader(config, bmc::system_io());
    EXPECT_EQ(reader->read(), 80);
    EXPECT_EQ(reader->read(), 150);
    EXPECT_EQ(reader->read(), 190);
    EXPECT_FALSE(reader->read());
    EXPECT_EQ(reader->read(), 80);
}
TEST(Reader, SysfsRejectsMalformedAndMissingData) {
    const auto path = temporary("-sensor");
    auto config = policy();
    config.backend = "sysfs";
    config.path = path.string();
    config.scale = 0.001;
    auto reader = bmc::make_reader(config, bmc::system_io());
    EXPECT_FALSE(reader->read());
    { std::ofstream output(path); output << "72000\n"; }
    EXPECT_EQ(reader->read(), 72);
    { std::ofstream output(path); output << "72000 garbage\n"; }
    EXPECT_FALSE(reader->read());
    std::filesystem::remove(path);
}
TEST(Logger, EscapesJsonAndRotates) {
    const auto path = temporary("-log");
    bmc::Logger logger(path, 100, 2);
    logger.action("sensor\"\n", "first");
    logger.action("sensor", "second");
    logger.action("sensor", "third");
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(path.string() + ".1"));
    EXPECT_EQ(bmc::escape("a\"\n\\"), "a\\\"\\n\\\\");
    for (const auto& suffix : {"", ".1", ".2"}) {
        std::filesystem::remove(path.string() + suffix);
    }
}
TEST(Worker, DrainsAndRejectsAfterStopping) {
    std::atomic<unsigned> completed = 0;
    bmc::Worker worker(20);
    for (unsigned iteration = 0; iteration < 10; ++iteration) {
        EXPECT_TRUE(worker.submit([&completed] { ++completed; }));
    }
    worker.stop();
    EXPECT_EQ(completed.load(), 10u);
    EXPECT_FALSE(worker.submit([] {}));
}
TEST(Config, RejectsDuplicateAndNegativeWindow) {
    const auto path = temporary("-config");
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 -1 3 -\n"; }
    EXPECT_THROW(bmc::load_config(path), std::invalid_argument);
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 3 3 -\ncpu mock 40 1 high 70 90 3 3 3 -\n"; }
    EXPECT_THROW(bmc::load_config(path), std::invalid_argument);
    std::filesystem::remove(path);
}
TEST(Pwm, ValidatesAndWritesConfiguredFile) {
    const auto path = temporary("-pwm");
    { std::ofstream output(path); output << "0\n"; }
    EXPECT_THROW(bmc::write_pwm(path.string(), 256, bmc::system_io()), std::invalid_argument);
    bmc::write_pwm(path.string(), 255, bmc::system_io());
    std::ifstream input(path);
    unsigned value = 0;
    input >> value;
    EXPECT_EQ(value, 255u);
    std::filesystem::remove(path);
}
// ============ Calibration：线性项、插值、钳制与配置解析 ============
TEST(Calibration, DefaultIsIdentityAndLinearTermsApply) {
    bmc::Calibration identity;
    EXPECT_DOUBLE_EQ(bmc::apply(identity, 42.5), 42.5);
    bmc::Calibration linear;
    linear.gain = 2;
    linear.offset = -3;
    EXPECT_DOUBLE_EQ(bmc::apply(linear, 10), 17);
    EXPECT_THROW(bmc::apply(identity, std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}
TEST(Calibration, InterpolatesBetweenPointsAndClampsOutside) {
    bmc::Calibration calibration;
    calibration.points = {{0, 100}, {10, 200}, {20, 260}};
    // 各校准点上取精确值。
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 0), 100);
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 10), 200);
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 20), 260);
    // 段内线性插值：0..10 段斜率 10，10..20 段斜率 6。
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 5), 150);
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 15), 230);
    // 区间外按端点钳制，不外推。
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, -5), 100);
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 100), 260);
}
TEST(Calibration, LinearTermsApplyBeforeInterpolation) {
    bmc::Calibration calibration;
    calibration.gain = 10;
    calibration.offset = 5;
    calibration.points = {{100, 1}, {200, 2}};
    // raw=5 -> 55 -> 落在首点之前 -> 钳制为 1；raw=15 -> 155 -> 位于 100..200 的 55% -> 1.55。
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 5), 1);
    EXPECT_DOUBLE_EQ(bmc::apply(calibration, 15), 1.55);
}
TEST(Calibration, ValidateRejectsUnsortedOrNonFinitePoints) {
    auto config = policy();
    config.calibration.points = {{10, 1}, {5, 2}};
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.calibration.points = {{5, 1}, {5, 2}};
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.calibration.points = {{5, std::numeric_limits<double>::infinity()}};
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.calibration.gain = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.calibration.points = {{5, 1}, {10, 2}};
    EXPECT_NO_THROW(bmc::validate(config));
}
// gain 必须为正：查表用的是 corrected = raw*gain+offset，负 gain 会让递增的标定点
// 在查表域里变成递减，区间查找随即落进错误的段并静默返回错误值。
TEST(Calibration, ValidateRejectsNonPositiveGain) {
    auto config = policy();
    config.calibration.gain = 0;
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.calibration.gain = -1;
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.calibration.gain = 0.5;
    config.calibration.points = {{5, 1}, {10, 2}};
    EXPECT_NO_THROW(bmc::validate(config));
}
TEST(Calibration, ConfigLineParsesOptionalTwelfthField) {
    const auto path = temporary("-calibration");
    // 旧格式（11 字段）保持可用，标定为默认恒等。
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 3 3 -\n"; }
    auto configs = bmc::load_config(path);
    ASSERT_EQ(configs.size(), 1u);
    EXPECT_DOUBLE_EQ(configs[0].calibration.gain, 1);
    EXPECT_TRUE(configs[0].calibration.points.empty());
    // 新格式：gain:offset;raw=value;raw=value
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 3 3 - 1.5:-2;0=10;100=210\n"; }
    configs = bmc::load_config(path);
    ASSERT_EQ(configs.size(), 1u);
    EXPECT_DOUBLE_EQ(configs[0].calibration.gain, 1.5);
    EXPECT_DOUBLE_EQ(configs[0].calibration.offset, -2);
    ASSERT_EQ(configs[0].calibration.points.size(), 2u);
    EXPECT_DOUBLE_EQ(configs[0].calibration.points[0].first, 0);
    EXPECT_DOUBLE_EQ(configs[0].calibration.points[1].second, 210);
    // "-" 与省略等价。
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 3 3 - -\n"; }
    configs = bmc::load_config(path);
    ASSERT_EQ(configs.size(), 1u);
    EXPECT_TRUE(configs[0].calibration.points.empty());
    // 乱序校准点在加载时即被拒绝。
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 3 3 - 1;50=1;10=2\n"; }
    EXPECT_THROW(bmc::load_config(path), std::invalid_argument);
    std::filesystem::remove(path);
}
// ============ Chips：各芯片驱动的寄存器解码与换算 ============
// 以下是专用芯片驱动的寄存器语义用例：只验证解码与换算，不验证电气行为。
// 脚本直接挂在 open() 会分配的 101 号描述符上。
namespace {
// 芯片用例的公共前置：登记 I2C_SLAVE 与能力位。能力位的实际内容取自 i2c_functions，
// 因此这里只需登记请求存在；需要"能力不足"的用例直接改 i2c_functions 即可。
void expect_chip_bus(FakeLinuxIo& fake) {
    fake.descriptors[101].replies[I2C_SLAVE] = {0};
    fake.descriptors[101].replies[I2C_FUNCS] = {0};
}
// SMBus word 读由内核还原字节序，因此这里按"低字节在前"给出寄存器内容。
std::vector<std::uint8_t> smbus_word_bytes(std::uint16_t value) {
    return {static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>(value >> 8)};
}
// 字/块读的回复以 ioctl 请求码（I2C_SMBUS）为键，word 内容用 smbus_word_bytes 构造。
// 注意：对 word 读而言 transaction->command 是寄存器号，只有字节读才以命令码为键。
void expect_reply(FakeLinuxIo& fake, unsigned long request, std::vector<std::uint8_t> bytes) {
    fake.descriptors[101].replies[request] = std::move(bytes);
}
// 字节读（read_byte）的回复以寄存器命令码为键；同一命令给多个元素表示连续多次读依次返回。
void expect_byte(FakeLinuxIo& fake, std::uint8_t command, std::vector<std::uint8_t> bytes) {
    fake.descriptors[101].replies[command] = std::move(bytes);
}
std::unique_ptr<bmc::I2cBus> make_bus(FakeLinuxIo& fake) {
    return std::make_unique<bmc::I2cBus>("/dev/fake-chip", 0x48, fake);
}
}
TEST(ChipLm75, DecodesElevenBitTwosComplementTemperature) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // 0x1F40 >> 5 == 250 -> 31.25 C
    expect_reply(fake, I2C_SMBUS, smbus_word_bytes(0x1F40));
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("lm75b", *bus, "");
    const auto sample = driver->read("temp");
    ASSERT_TRUE(sample);
    EXPECT_DOUBLE_EQ(*sample, 31.25);
    // 0xFFF8 >> 5 == -1 -> -0.125 C
    fake.descriptors[101].replies[I2C_SMBUS] = smbus_word_bytes(0xFFF8);
    const auto negative = driver->read("temp");
    ASSERT_TRUE(negative);
    EXPECT_DOUBLE_EQ(*negative, -0.125);
}
TEST(ChipLm75, NineBitVariantUsesHalfDegreeSteps) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // 9 位：0xFF00 >> 7 == -2 -> -1.0 C；0x0180 >> 7 == 3 -> 1.5 C（低 7 位为精度位，本实现保留）。
    expect_reply(fake, I2C_SMBUS, smbus_word_bytes(0xFF00));
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("lm75", *bus, "");
    const auto sample = driver->read("temp");
    ASSERT_TRUE(sample);
    EXPECT_DOUBLE_EQ(*sample, -1.0);
    fake.descriptors[101].replies[I2C_SMBUS] = smbus_word_bytes(0x0180);
    const auto half = driver->read("temp");
    ASSERT_TRUE(half);
    EXPECT_DOUBLE_EQ(*half, 1.5);
}
TEST(ChipSelection, RejectsUnknownChipAndUnsupportedFeature) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    auto bus = make_bus(fake);
    EXPECT_THROW(bmc::make_chip("lm99", *bus, ""), std::invalid_argument);
    // lm75 只有 temp，没有 bus。
    EXPECT_THROW(bmc::make_chip("lm75", *bus, "bus"), std::invalid_argument);
    EXPECT_NO_THROW(bmc::make_chip("lm75b", *bus, "temp"));
}
TEST(ChipAdm1275, AppliesTwelveBitMaskAndHalfCodeOffset) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // READ_VIN 0x88：高 4 位保留，取 bit11:0；0x1234 & 0x0FFF = 0x234 = 564。
    fake.descriptors[101].replies[I2C_SMBUS] = smbus_word_bytes(0x1234);
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("adm1275", *bus, "");
    const auto voltage = driver->read("vin");
    ASSERT_TRUE(voltage);
    // 默认 0-20 V 量程的 LSB 为 5.208 mV，且电压采用 code + 0.5 偏移。
    EXPECT_NEAR(*voltage, 5.208e-3 * 564.5, 1e-9);
    // 未配置分流电阻时无法给出电流的安培值。
    fake.descriptors[101].replies[I2C_SMBUS] = smbus_word_bytes(0x0800);
    EXPECT_FALSE(driver->read("iout"));
    // 该芯片没有功率与温度命令。
    EXPECT_FALSE(driver->read("power"));
    EXPECT_FALSE(driver->read("temp"));
}
TEST(ChipIna219, ProgrammesCalibrationAndShiftsBusVoltage) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // 母线电压数据在 bit15:3，bit1=CNVR、bit0=OVF 必须被右移丢弃。
    expect_reply(fake, I2C_SMBUS, smbus_word_bytes(static_cast<std::uint16_t>((3000u << 3) | 0x3)));
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("ina219", *bus, "bus");
    const auto voltage = driver->read("bus");
    ASSERT_TRUE(voltage);
    EXPECT_DOUBLE_EQ(*voltage, 12.0);  // 3000 * 4 mV
    // 构造时应写入默认校准值 4096，且 INA219 的 FS0 恒为 0。
    ASSERT_EQ(fake.descriptors[101].registers.count(0x05), 1u);
    EXPECT_EQ(fake.descriptors[101].registers[0x05], 4096u);
    EXPECT_EQ(fake.smbus_writes, 1);
}
TEST(ChipIna219, ShuntVoltageIsSigned) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    expect_reply(fake, I2C_SMBUS, smbus_word_bytes(static_cast<std::uint16_t>(-100)));
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("ina219", *bus, "shunt");
    const auto shunt = driver->read("shunt");
    ASSERT_TRUE(shunt);
    EXPECT_DOUBLE_EQ(*shunt, -100 * 1e-5);  // -1 mV
}
TEST(ChipIna226, UsesFifteenBitBusAndTwentyFivePowerFactor) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // INA226 母线电压为 15 位无符号，不需要移位。
    expect_reply(fake, I2C_SMBUS, smbus_word_bytes(16000));
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("ina226", *bus, "bus");
    const auto voltage = driver->read("bus");
    ASSERT_TRUE(voltage);
    EXPECT_DOUBLE_EQ(*voltage, 20.0);  // 16000 * 1.25 mV
    ASSERT_EQ(fake.descriptors[101].registers.count(0x05), 1u);
    EXPECT_EQ(fake.descriptors[101].registers[0x05], 2048u);
    // 默认校准下 Current_LSB 等于分流 LSB（2.5 uV），功率 LSB 为其 25 倍。
    fake.descriptors[101].replies[I2C_SMBUS] = smbus_word_bytes(0);
    auto power_driver = bmc::make_chip("ina226", *bus, "power");
    fake.descriptors[101].replies[I2C_SMBUS] = smbus_word_bytes(100);
    const auto power = power_driver->read("power");
    ASSERT_TRUE(power);
    EXPECT_DOUBLE_EQ(*power, 100 * 25 * 2.5e-6);
}
TEST(ChipEmc2103, ConvertsTachCountWithTheDatasheetConstant) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // 高字节 0x0F、低字节 0x00 -> COUNT = (0x0F << 5) | 0 = 480。
    // RPM = 3932160 * 2 / 480 = 16384（默认 1000 RPM 量程，m = 2）。
    expect_byte(fake, 0x4E, {0x0F});
    expect_byte(fake, 0x4F, {0x00});    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("emc2103", *bus, "rpm");
    const auto rpm = driver->read("rpm");
    ASSERT_TRUE(rpm);
    EXPECT_DOUBLE_EQ(*rpm, 3932160.0 * 2 / 480);
}
TEST(ChipEmc2103, StoppedTachSentinelAndZeroAreNotSpeeds) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // 0x1FE0 是寄存器默认值，表示风扇停转或未接：COUNT = (0xFF << 5) | (0xF8 >> 3) = 0x1FE0。
    expect_byte(fake, 0x4E, {0xFF});
    expect_byte(fake, 0x4F, {0xF8});
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("emc2103", *bus, "rpm");
    EXPECT_FALSE(driver->read("rpm"));
}
TEST(ChipEmc2103, TemperatureIsSixteenthDegreeAndDiodeFaultIsNotATemperature) {
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    expect_byte(fake, 0x00, {0x1F});  // internal temperature high byte
    expect_byte(fake, 0x01, {0x40});  // low byte -> 0x1F40 / 256 = 31.25 C
    auto bus = make_bus(fake);
    auto driver = bmc::make_chip("emc2103", *bus, "temp");
    const auto warm = driver->read("temp");
    ASSERT_TRUE(warm);
    EXPECT_DOUBLE_EQ(*warm, 31.25);
    // 负温度：0xFF00 / 256 = -1.0 C
    fake.descriptors[101].replies[0x00] = {0xFF};
    fake.descriptors[101].replies[0x01] = {0x00};
    const auto cold = driver->read("temp");
    ASSERT_TRUE(cold);
    EXPECT_DOUBLE_EQ(*cold, -1.0);
    // 0x8000 表示二极管故障，必须报"无读数"而不是 -128 C。
    fake.descriptors[101].replies[0x00] = {0x80};
    fake.descriptors[101].replies[0x01] = {0x00};
    EXPECT_FALSE(driver->read("temp"));
}
TEST(ChipConfig, ChipBackendParsesAndRejectsUnknownNames) {
    const auto path = temporary("-chip");
    { std::ofstream output(path); output << "temp i2c:lm75b /dev/i2c-1,0x48 1 high 70 90 3 3 3 -\n"; }
    auto configs = bmc::load_config(path);
    ASSERT_EQ(configs.size(), 1u);
    EXPECT_EQ(configs[0].backend, "i2c:lm75b");
    { std::ofstream output(path); output << "temp i2c:lm99 /dev/i2c-1,0x48 1 high 70 90 3 3 3 -\n"; }
    EXPECT_THROW(bmc::load_config(path), std::invalid_argument);
    // 后端语法是 i2c:<chip>[@<feature>]：型号名只取 "@" 之前的部分，
    // 测量量留给构造期 make_chip 校验，因此这些写法都必须在装载阶段通过。
    { std::ofstream output(path); output << "fan i2c:emc2103@rpm /dev/i2c-1,0x2E 1 low 500 200 20 3 3 -\n"; }
    configs = bmc::load_config(path);
    ASSERT_EQ(configs.size(), 1u);
    EXPECT_EQ(configs[0].backend, "i2c:emc2103@rpm");
    { std::ofstream output(path); output << "vout i2c:adm1275@vout /dev/i2c-1,0x10 1 high 12 13 0.5 3 3 -\n"; }
    EXPECT_NO_THROW(bmc::load_config(path));
    // 带 @ 也不能放过真正未知的型号。
    { std::ofstream output(path); output << "temp i2c:lm99@temp /dev/i2c-1,0x48 1 high 70 90 3 3 3 -\n"; }
    EXPECT_THROW(bmc::load_config(path), std::invalid_argument);
    { std::ofstream output(path); output << "temp i2c:lm75 /dev/i2c-1,0x48,0x00 1 high 70 90 3 3 3 -\n"; }
    configs = bmc::load_config(path);
    ASSERT_EQ(configs.size(), 1u);
    FakeLinuxIo fake;
    expect_chip_bus(fake);
    // 芯片后端的 path 只接受 device,address，多给寄存器字段即报错。
    EXPECT_THROW(bmc::make_reader(configs[0], fake), std::invalid_argument);
    std::filesystem::remove(path);
}
// ============ FakeIo / Device：注入式 I/O 与设备适配 ============
// 以下用例验证"注入后的调用链"，而不是真实硬件行为。
std::uint16_t decode_i2c_word(const std::vector<std::uint8_t>& bytes) {
    std::uint16_t word = 0;
    std::memcpy(&word, bytes.data(), sizeof(word));
    return word;
}
unsigned long read_i2c_functions(FakeLinuxIo& fake) {
    unsigned long value = 0;
    const auto& bytes = fake.descriptors[fake.path_descriptors["/dev/fake-i2c"]].replies.at(I2C_FUNCS);
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}
// 只有伪 Io 真的返回 count-1，write_pwm 的短写检测分支才被覆盖到。
TEST(FakeIo, RequiresARealShortWriteToBeDetectable) {
    FakeLinuxIo fake;
    bmc::write_pwm("/dev/fake-pwm", 200, fake);
    auto& entry = fake.descriptors[fake.path_descriptors["/dev/fake-pwm"]];
    ASSERT_EQ(entry.writes.size(), 1u);
    EXPECT_EQ(entry.writes[0], "200\n");
    entry.short_write = true;
    EXPECT_THROW(bmc::write_pwm("/dev/fake-pwm", 201, fake), std::runtime_error);
}
TEST(FakeIo, PwmActionWritesThroughInjectedIo) {
    FakeLinuxIo fake;
    bmc::PwmAction action(fake);
    EXPECT_TRUE(action.execute({"rule", "cpu", "inspect_device", 1, ""}));
    EXPECT_TRUE(action.execute({"rule", "cpu", "increase_fan", 2, "/dev/fake-pwm"}));
    ASSERT_EQ(fake.descriptors[fake.path_descriptors["/dev/fake-pwm"]].writes.size(), 1u);
    EXPECT_EQ(fake.descriptors[fake.path_descriptors["/dev/fake-pwm"]].writes[0], "255\n");
    EXPECT_FALSE(action.execute({"rule", "cpu", "increase_fan", 3, ""}));
    EXPECT_FALSE(action.execute({"rule", "cpu", "power_cycle", 4, "/dev/fake-pwm"}));
}
TEST(FakeIo, I2cReaderPropagatesOpenAndCapabilityFailures) {
    FakeLinuxIo fake;
    auto config = policy();
    config.backend = "i2c";
    config.path = "/dev/fake-open-failure,0x48,0x00";
    EXPECT_THROW(bmc::make_reader(config, fake), std::system_error);
    FakeLinuxIo incapable;
    incapable.descriptors[101].replies[I2C_SLAVE] = {0};
    incapable.i2c_functions = 0;
    config.path = "/dev/fake-i2c,0x48,0x00";
    EXPECT_THROW(bmc::make_reader(config, incapable), std::runtime_error);
}
TEST(FakeIo, I2cReaderDecodesWordAndScalesIt) {
    FakeLinuxIo fake;
    const unsigned long smbus_read_word = I2C_FUNC_SMBUS_READ_WORD_DATA;
    std::vector<std::uint8_t> capabilities(sizeof(smbus_read_word));
    std::memcpy(capabilities.data(), &smbus_read_word, sizeof(smbus_read_word));
    fake.descriptors[101].replies[I2C_SLAVE] = {0};
    fake.descriptors[101].replies[I2C_FUNCS] = capabilities;
    const std::uint16_t raw = 0x1234;
    std::vector<std::uint8_t> word(sizeof(raw));
    std::memcpy(word.data(), &raw, sizeof(raw));
    fake.descriptors[101].replies[I2C_SMBUS] = word;
    auto config = policy();
    config.backend = "i2c";
    config.path = "/dev/fake-i2c,0x48,0x00";
    config.scale = 0.0625;
    auto reader = bmc::make_reader(config, fake);
    const auto sample = reader->read();
    ASSERT_TRUE(sample);
    EXPECT_DOUBLE_EQ(*sample, 0x1234 * 0.0625);
    EXPECT_EQ(decode_i2c_word(fake.descriptors[101].replies[I2C_SMBUS]), raw);
    EXPECT_EQ(read_i2c_functions(fake), smbus_read_word);
    EXPECT_EQ(fake.selected_slaves, 1);
    EXPECT_EQ(fake.opened.size(), 1u);
    EXPECT_EQ(fake.opened[0], "/dev/fake-i2c");
}
// 行值来自脚本化 ioctl，但描述符是真实的 /dev/null：CLOEXEC 与析构时关闭都真实生效。
TEST(FakeIo, GpioLineValuesFailureAndDescriptorLifetime) {
    FakeLinuxIo fake;
    auto config = policy(); config.backend = "gpio"; config.path = "/dev/fake-gpiochip,1,active-low";
    config.scale = 2;
    auto reader = bmc::make_reader(config, fake);
    ASSERT_GE(fake.line_descriptor, 0);
    EXPECT_NE(::fcntl(fake.line_descriptor, F_GETFD) & FD_CLOEXEC, 0);
    ASSERT_EQ(fake.line_requests.size(), 1u);
    EXPECT_NE(fake.line_requests[0] & GPIO_V2_LINE_FLAG_ACTIVE_LOW, 0u);
    EXPECT_EQ(reader->read(), 2);
    fake.descriptors[fake.line_descriptor].replies[GPIO_V2_LINE_GET_VALUES_IOCTL] = {0};
    EXPECT_EQ(reader->read(), 0);
    fake.descriptors.erase(fake.line_descriptor);
    EXPECT_FALSE(reader->read());
    const auto descriptor = fake.line_descriptor;
    reader.reset(); errno = 0;
    EXPECT_EQ(::fcntl(descriptor, F_GETFD), -1); EXPECT_EQ(errno, EBADF);
}
TEST(FakeIo, GpioReaderRejectsOffsetOutsideChipBeforeRequestingTheLine) {
    FakeLinuxIo fake;
    fake.gpio_lines = 8;
    fake.descriptors[101].replies[GPIO_GET_CHIPINFO_IOCTL] = {0};
    auto config = policy();
    config.backend = "gpio";
    config.path = "/dev/fake-gpiochip,8";
    EXPECT_THROW(bmc::make_reader(config, fake), std::invalid_argument);
}
TEST(Device, RegistryProvidesLifecycleAndInventory) {
    auto config = policy();
    auto device = bmc::make_device(config, bmc::system_io());
    EXPECT_EQ(device->state(), bmc::DeviceState::closed);
    EXPECT_TRUE(device->open());
    EXPECT_EQ(device->state(), bmc::DeviceState::ready);
    EXPECT_TRUE(device->probe());
    EXPECT_EQ(device->read_value(), 75);
    bmc::DeviceRegistry registry;
    registry.add(std::move(device));
    ASSERT_NE(registry.find("cpu"), nullptr);
    ASSERT_EQ(registry.inventory().size(), 1u);
    EXPECT_TRUE(registry.open_all());
    registry.close_all();
    EXPECT_EQ(registry.find("cpu")->state(), bmc::DeviceState::closed);
}
TEST(Device, RegistryRejectsDuplicateIds) {
    auto config = policy();
    bmc::DeviceRegistry registry;
    registry.add(bmc::make_device(config, bmc::system_io()));
    EXPECT_THROW(registry.add(bmc::make_device(config, bmc::system_io())), std::invalid_argument);
}
TEST(Device, SysfsAdapterReportsDegradedState) {
    auto config = policy();
    config.backend = "sysfs";
    config.path = "/definitely/missing/bmc-sensor";
    auto device = bmc::make_device(config, bmc::system_io());
    EXPECT_FALSE(device->open());
    EXPECT_EQ(device->state(), bmc::DeviceState::failed);
    EXPECT_FALSE(device->probe());
}
// ============ EventBus / Rules / Recovery：事件分发、策略与恢复 ============
TEST(EventBus, DeliversFilteredEventsAndSequencesThem) {
    bmc::EventBus bus(8);
    std::mutex mutex;
    std::vector<bmc::BusEvent> received;
    bus.subscribe(bmc::BusEventType::sensor_state, [&mutex, &received](const bmc::BusEvent& event) {
        std::lock_guard lock(mutex); received.push_back(event);
    });
    EXPECT_TRUE(bus.publish({bmc::BusEventType::configuration, "cfg", "ignored", std::nullopt}));
    EXPECT_TRUE(bus.publish({bmc::BusEventType::sensor_state, "cpu", "critical", 95.0}));
    bus.stop();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].source, "cpu");
    EXPECT_EQ(received[0].sequence, 2u);
}
TEST(EventBus, DropsWhenFullAndRejectsAfterStop) {
    bmc::EventBus bus(1);
    std::promise<void> blocker;
    auto future = blocker.get_future();
    bus.subscribe(bmc::BusEventType::service, [&future](const bmc::BusEvent&) { future.wait(); });
    EXPECT_TRUE(bus.publish({bmc::BusEventType::service, "s", "first", std::nullopt}));
    for (unsigned index = 0; index < 100; ++index) {
        bus.publish({bmc::BusEventType::service, "s", "queued", std::nullopt});
    }
    EXPECT_GT(bus.dropped(), 0u);
    blocker.set_value();
    bus.stop();
    EXPECT_FALSE(bus.publish({bmc::BusEventType::service, "s", "stopped", std::nullopt}));
}
TEST(Rules, ActivatesAfterConfirmationsAndClearsSeparately) {
    bmc::FaultRule rule{"cpu", "cpu", bmc::State::critical, 2, 2, "increase_fan"};
    bmc::FaultRuleEngine engine({rule});
    bmc::Event first{"cpu", bmc::State::normal, bmc::State::critical, 95, "threshold", 1};
    EXPECT_TRUE(engine.evaluate(first).empty());
    auto activated = engine.evaluate(first);
    ASSERT_EQ(activated.size(), 1u);
    EXPECT_TRUE(activated[0].active);
    bmc::Event normal{"cpu", bmc::State::critical, bmc::State::normal, 40, "threshold", 2};
    EXPECT_TRUE(engine.evaluate(normal).empty());
    auto cleared = engine.evaluate(normal);
    ASSERT_EQ(cleared.size(), 1u);
    EXPECT_FALSE(cleared[0].active);
    EXPECT_EQ(cleared[0].action, "clear");
}
TEST(Recovery, AppliesCooldownRetriesAndReset) {
    unsigned calls = 0;
    bmc::RecoveryPolicyEngine engine([&](const bmc::RecoveryRequest&) { ++calls; return calls >= 2; }, std::chrono::hours(1), 3, 1);
    auto first = engine.submit({"r", "cpu", "increase_fan", 1});
    ASSERT_TRUE(first); EXPECT_TRUE(first->accepted); EXPECT_TRUE(first->success); EXPECT_EQ(first->attempts, 2u);
    auto blocked = engine.submit({"r", "cpu", "increase_fan", 2});
    ASSERT_TRUE(blocked); EXPECT_FALSE(blocked->accepted); EXPECT_EQ(blocked->detail, "cooldown");
    engine.reset("cpu");
    auto second = engine.submit({"r", "cpu", "increase_fan", 3});
    ASSERT_TRUE(second); EXPECT_TRUE(second->accepted);
}
TEST(Recovery, SeparateWorkerCanRunWhileAnotherActionWaits) {
    bmc::Worker worker(4, 2);
    std::promise<void> entered;
    std::promise<void> release;
    std::promise<void> completed;
    auto release_signal = release.get_future().share();
    auto completed_signal = completed.get_future();
    bmc::RecoveryPolicyEngine recovery([&](const bmc::RecoveryRequest& request) {
        if (request.sensor == "slow") {
            entered.set_value();
            release_signal.wait();
        } else {
            completed.set_value();
        }
        return true;
    }, std::chrono::milliseconds(0), 1, 2);
    ASSERT_TRUE(worker.submit([&] { recovery.submit({"rule", "slow", "inspect_device", 1}); }));
    entered.get_future().wait();
    ASSERT_TRUE(worker.submit([&] { recovery.submit({"rule", "fast", "inspect_device", 2}); }));
    const auto status = completed_signal.wait_for(std::chrono::seconds(1));
    release.set_value();
    worker.stop();
    EXPECT_EQ(status, std::future_status::ready);
}
TEST(Recovery, SameKeyAlreadyRunningIsReportedSeparatelyFromTheConcurrencyCap) {
    // 同一（传感器, 动作）重入属于去重，不是并发额度不足；两种拒绝必须给出不同 detail，
    // 否则按 detail 分类的调用方会把重入误判成扩容信号。
    std::promise<void> entered, release;
    auto hold = release.get_future().share();
    bmc::RecoveryPolicyEngine recovery([&](const bmc::RecoveryRequest& request) {
        if (request.sensor != "slow") return true;
        entered.set_value();
        hold.wait();
        return true;
    }, std::chrono::milliseconds(0), 1, 2);
    bmc::Worker worker(4, 2);
    ASSERT_TRUE(worker.submit([&] { recovery.submit({"rule", "slow", "increase_fan", 1}); }));
    entered.get_future().wait();
    // 执行器已被占用，但这是同一把键的重入：去重分支先命中。
    const auto duplicate = recovery.submit({"rule", "slow", "increase_fan", 2});
    ASSERT_TRUE(duplicate);
    EXPECT_FALSE(duplicate->accepted);
    EXPECT_EQ(duplicate->detail, "already running");
    release.set_value();
    worker.stop();
}
TEST(Recovery, ManyFailuresKeepTotalBackoffBounded) {
    bmc::RecoveryPolicyEngine recovery([](const bmc::RecoveryRequest&) { return false; },
        std::chrono::milliseconds(0), 8, 1);
    const auto started = std::chrono::steady_clock::now();
    const auto result = recovery.submit({"rule", "cpu", "inspect_device", 1});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(result);
    EXPECT_EQ(result->attempts, 8u);
    EXPECT_LT(elapsed, std::chrono::milliseconds(900));
}
TEST(Recovery, RejectsInvalidPolicy) {
    EXPECT_THROW(bmc::RecoveryPolicyEngine(nullptr), std::invalid_argument);
    EXPECT_THROW(bmc::RecoveryPolicyEngine([](const bmc::RecoveryRequest&) { return true; }, std::chrono::seconds(1), 0), std::invalid_argument);
}
// 传感器反复出现又消失时冷却状态不能无限增长，总量始终被 4096 条上限兜住。
TEST(Recovery, DisappearingSensorsHaveBoundedCooldownState) {
    bmc::RecoveryPolicyEngine recovery([](const auto&) { return true; }, std::chrono::milliseconds(0), 1);
    for (unsigned index = 0; index < 10000; ++index) {
        const auto result = recovery.submit({"rule", "sensor-" + std::to_string(index), "inspect_device", index});
        ASSERT_TRUE(result && result->success);
        EXPECT_LE(recovery.runtime_size(), 4096u);
    }
}
// 容量满时新传感器以 "state capacity" 被拒，但已入表传感器的冷却语义不受影响。
TEST(Recovery, StateCapacityPreservesExistingCooldowns) {
    bmc::RecoveryPolicyEngine recovery([](const auto&) { return true; }, std::chrono::hours(1), 1);
    for (unsigned index = 0; index < 4096; ++index)
        ASSERT_TRUE(recovery.submit({"rule", "sensor-" + std::to_string(index), "inspect_device", index})->success);
    EXPECT_EQ(recovery.runtime_size(), 4096u);
    EXPECT_EQ(recovery.submit({"rule", "extra", "inspect_device", 0})->detail, "state capacity");
    EXPECT_EQ(recovery.submit({"rule", "sensor-0", "inspect_device", 0})->detail, "cooldown");
}
// ============ Sel / Logger：持久化加固与写失败降级 ============
TEST(Sel, PersistsSequencesAndLimitsRecords) {
    const auto path = temporary("-sel.db");
    { bmc::SelStore store(path, 2); EXPECT_EQ(store.append("cpu", "critical", "hot", 95), 1u); EXPECT_EQ(store.append("cpu", "normal", "clear"), 2u); EXPECT_EQ(store.append("fan", "failed", "stalled"), 3u); EXPECT_EQ(store.query().size(), 2u); }
    bmc::SelStore restored(path, 2);
    ASSERT_EQ(restored.query().size(), 2u);
    EXPECT_EQ(restored.query()[0].id, 2u);
    EXPECT_EQ(restored.next_id(), 4u);
    std::filesystem::remove(path);
}
// 压实必须幂等：反复重开并压实后既不能丢记录，也不能让同一记录重复落盘。
TEST(Sel, AppendsAfterRepeatedCompactionPersistExactlyOnce) {
    const auto path = temporary("-sel-compact-reopen.db");
    {
        bmc::SelStore store(path, 3);
        for (unsigned index = 0; index < 20; ++index) {
            store.append("cpu", "normal", "sample-" + std::to_string(index), std::nullopt, true);
            ASSERT_TRUE(store.flush());
            bmc::SelStore reader(path, 3);
            const auto records = reader.query();
            ASSERT_EQ(records.size(), std::min<std::size_t>(index + 1, 3));
            EXPECT_EQ(records.back().id, index + 1);
            EXPECT_EQ(reader.next_id(), index + 2);
        }
    }
    std::ifstream input(path);
    std::string line; unsigned count = 0;
    while (std::getline(input, line)) ++count;
    EXPECT_EQ(count, 3u);
    std::filesystem::remove(path);
}
TEST(Sel, BatchesWritesAndFlushIsMeaningful) {
    const auto path = temporary("-sel-batch.db");
    std::filesystem::remove(path);
    // 把 stdout 重定向到 /dev/null，用真实写入路径测试批量语义。
    std::fflush(stdout);
    const int saved = ::dup(STDOUT_FILENO);
    ASSERT_GE(saved, 0);
    const int null_fd = ::open("/dev/null", O_WRONLY);
    ASSERT_GE(null_fd, 0);
    ASSERT_NE(::dup2(null_fd, STDOUT_FILENO), -1);
    ::close(null_fd);
    {
        bmc::SelStore store(path, 4096);
        // 单条记录远小于批次阈值，写完后应当仍在缓冲区里。
        store.append("cpu", "normal", "sample");
        EXPECT_GT(store.pending_bytes(), 0u);
        EXPECT_TRUE(store.flush());
        EXPECT_EQ(store.pending_bytes(), 0u);
        EXPECT_EQ(store.write_failures(), 0u);
        // 累计超过 4096 字节后自动落盘。
        for (unsigned index = 0; index < 200; ++index) {
            store.append("cpu", "normal", "sample-" + std::to_string(index));
        }
        EXPECT_LT(store.pending_bytes(), 4096u);
    }
    std::fflush(stdout);
    ASSERT_NE(::dup2(saved, STDOUT_FILENO), -1);
    ::close(saved);
    // 重启后应当读回全部 201 条记录。
    bmc::SelStore reopened(path, 4096);
    // query() defaults to the most recent 100 records, so ask for all of them.
    EXPECT_EQ(reopened.query(500).size(), 201u);
    std::filesystem::remove(path);
}
TEST(Sel, TruncatesTornTailOnStartupAndReportsIt) {
    const auto path = temporary("-sel-tail.db");
    // 一个完整记录 + 一条崩溃留下的残缺记录（没有换行结尾）。
    {
        std::ofstream output(path, std::ios::binary);
        output << "1 1000 \"cpu\" \"critical\" \"hot\" 95\n2 2000 \"cpu\" \"normal\" \"partial";
    }
    const auto before = std::filesystem::file_size(path);
    bmc::SelStore store(path);
    // 只有完整记录被重放，残缺尾部按截断处理。
    ASSERT_EQ(store.query().size(), 1u);
    EXPECT_EQ(store.query()[0].id, 1u);
    EXPECT_GT(store.truncated_bytes(), 0u);
    EXPECT_LT(std::filesystem::file_size(path), before);
    // 截断后文件可以继续正常追加，并且不会破坏已有记录。
    store.append("fan", "failed", "stalled", std::nullopt, true);
    EXPECT_TRUE(store.flush());
    EXPECT_EQ(store.truncated_bytes(), std::string("2 2000 \"cpu\" \"normal\" \"partial").size());
    bmc::SelStore reopened(path);
    ASSERT_EQ(reopened.query().size(), 2u);
    EXPECT_EQ(reopened.query()[1].source, "fan");
    EXPECT_EQ(reopened.next_id(), 3u);
    std::filesystem::remove(path);
}
TEST(Sel, RefusePolicyKeepsTornTailOnDisk) {
    const auto path = temporary("-sel-refuse.db");
    {
        std::ofstream output(path, std::ios::binary);
        output << "1 1000 \"cpu\" \"critical\" \"hot\" 95\n2 2000 \"cpu\" \"normal\" \"partial";
    }
    const auto before = std::filesystem::file_size(path);
    bmc::SelStore store(path, 4096, bmc::Truncate::refuse);
    EXPECT_GT(store.truncated_bytes(), 0u);
    // refuse 不修改文件，残缺行原样保留。
    EXPECT_EQ(std::filesystem::file_size(path), before);
    std::filesystem::remove(path);
}
TEST(Sel, SyncFailureIsCountedSeparatelyFromWriteFailure) {
    // /dev/null 是字符设备，不执行 fdatasync；普通文件的失败由下方注入回调验证。
    {
        bmc::SelStore store("/dev/null", 4096);
        EXPECT_NO_THROW(store.append("cpu", "normal", "sample", std::nullopt, true));
        EXPECT_EQ(store.write_failures(), 0u);
        EXPECT_EQ(store.sync_failures(), 0u);
        EXPECT_TRUE(store.flush());
    }
    // 普通文件才会调用 fdatasync；注入 EIO 验证独立计数。
    const auto path = temporary("-sel-sync.db");
    {
        unsigned calls = 0;
        bmc::SelStore store(path, 4096, bmc::Truncate::tail, [&](int) {
            ++calls;
            errno = EIO;
            return -1;
        });
        store.append("cpu", "normal", "sample", std::nullopt, true);
        EXPECT_GT(calls, 0u);
        EXPECT_EQ(store.write_failures(), 0u);
        EXPECT_GT(store.sync_failures(), 0u);
        EXPECT_TRUE(store.flush());
    }
    std::filesystem::remove(path);
}
TEST(Sel, WriteFailureDegradesInsteadOfTerminating) {
    // 用一个"无法再写入"的目标制造稳定失败：先打开管道写端并把容量填满，
    // 再让 SelStore 以非阻塞方式打开同一路径（/proc/self/fd/N），
    // 此时每次 write 都以 EAGAIN 失败。这比 /dev/full 更可移植（容器里常常没有该设备）。
    int ends[2] = {-1, -1};
    ASSERT_EQ(::pipe(ends), 0);
    ASSERT_NE(::fcntl(ends[1], F_SETFL, ::fcntl(ends[1], F_GETFL) | O_NONBLOCK), -1);
    const std::string filler(4096, 'x');
    for (;;) {
        const auto written = ::write(ends[1], filler.data(), filler.size());
        if (written <= 0) {
            break;
        }
    }
    const std::string target = "/proc/self/fd/" + std::to_string(ends[1]);
    {
        bmc::SelStore store(target, 4096);
        // 调小上限，就能在不写 1 MiB 数据的前提下验证"超限丢弃最旧内容"。
        store.set_pending_cap(16);
        for (unsigned index = 0; index < 4; ++index) {
            EXPECT_NO_THROW(store.append("cpu", "critical", "hot", 95, true));
        }
        EXPECT_GT(store.write_failures(), 0u);
        EXPECT_FALSE(store.flush());
        EXPECT_LE(store.pending_bytes(), 16u);
        EXPECT_GT(store.dropped_bytes(), 0u);
    }
    ::close(ends[0]);
    ::close(ends[1]);
}
TEST(Logger, BatchesWritesIntoFewerSyscalls) {
    const auto path = temporary("-log-batch");
    std::filesystem::remove(path);
    {
        // 一条 action 记录约 61 字节；批次阈值 96 意味着第二条记录会触发落盘。
        bmc::Logger logger(path, 1u << 20, 2, false, 96);
        logger.action("sensor", "first");
        // 单条记录不足以触发批次阈值，应仍在缓冲区。
        EXPECT_GT(logger.pending_bytes(), 0u);
        EXPECT_EQ(logger.write_failures(), 0u);
        // 第二条把缓冲区推过阈值，自动落盘。
        logger.action("sensor", "second");
        EXPECT_EQ(logger.pending_bytes(), 0u);
        EXPECT_EQ(logger.write_failures(), 0u);
        logger.action("sensor", "third");
        EXPECT_GT(logger.pending_bytes(), 0u);
        EXPECT_TRUE(logger.flush());
        EXPECT_EQ(logger.pending_bytes(), 0u);
    }
    std::ifstream input(path);
    int lines = 0;
    std::string line;
    while (std::getline(input, line)) {
        EXPECT_EQ(line.front(), '{');
        EXPECT_EQ(line.back(), '}');
        ++lines;
    }
    EXPECT_EQ(lines, 3);
    std::filesystem::remove(path);
}
TEST(Logger, SyncPolicyWritesEveryRecordImmediately) {
    const auto path = temporary("-log-sync");
    std::filesystem::remove(path);
    {
        // sync=true 等价于旧行为：每条记录立即落盘。
        bmc::Logger logger(path, 1u << 20, 2, true);
        logger.action("sensor", "first");
        EXPECT_EQ(logger.pending_bytes(), 0u);
        EXPECT_EQ(logger.write_failures(), 0u);
        logger.action("sensor", "second");
        EXPECT_EQ(logger.pending_bytes(), 0u);
    }
    bmc::Logger reopened(path, 1u << 20, 2, true);
    EXPECT_EQ(reopened.write_failures(), 0u);
    std::ifstream input(path);
    int lines = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++lines;
    }
    EXPECT_EQ(lines, 2);
    std::filesystem::remove(path);
}
TEST(Logger, BoundedPendingAndFailureCounting) {
    const auto path = temporary("-log-pending");
    std::filesystem::remove(path);
    {
        // 批次阈值设得极大，记录只会堆在缓冲区里，便于观察上限与失败计数。
        bmc::Logger logger(path, 1u << 20, 2, false, 1u << 20);
        const std::string filler(1024, 'x');
        for (unsigned index = 0; index < 256; ++index) {
            EXPECT_NO_THROW(logger.action("sensor", "entry-" + std::to_string(index) + filler));
        }
        EXPECT_EQ(logger.write_failures(), 0u);
        EXPECT_TRUE(logger.flush());
        EXPECT_EQ(logger.pending_bytes(), 0u);
        EXPECT_EQ(logger.dropped_bytes(), 0u);
    }
    std::filesystem::remove(path);
}
TEST(Logger, FullDeviceDropsOldestPendingBytes) {
    bmc::Logger logger("/dev/full", 1u << 20, 2, false, 1024);
    const std::string filler(2048, 'x');
    for (unsigned index = 0; index < 600; ++index)
        logger.action("sensor", filler);
    EXPECT_LE(logger.pending_bytes(), 1u << 20);
    EXPECT_GT(logger.dropped_bytes(), 0u);
    EXPECT_GT(logger.write_failures(), 0u);
}
TEST(Logger, WriteFailureIsCountedAndNeverThrows) {
    // 填满的管道 + O_NONBLOCK：每次 write 都以 EAGAIN 失败。日志不再逐行抛异常终止服务，
    // 而是计入失败次数；SEL 侧的同名语义已经覆盖，这里确认日志侧一致。
    int ends[2] = {-1, -1};
    ASSERT_EQ(::pipe(ends), 0);
    ASSERT_NE(::fcntl(ends[1], F_SETFL, ::fcntl(ends[1], F_GETFL) | O_NONBLOCK), -1);
    const std::string filler(4096, 'x');
    for (;;) {
        const auto written = ::write(ends[1], filler.data(), filler.size());
        if (written <= 0) {
            break;
        }
    }
    const std::string target = "/proc/self/fd/" + std::to_string(ends[1]);
    {
        bmc::Logger logger(target, 1u << 20, 2);
        for (unsigned index = 0; index < 3; ++index) {
            EXPECT_NO_THROW(logger.action("sensor", "entry-" + std::to_string(index)));
        }
        EXPECT_GT(logger.write_failures(), 0u);
        EXPECT_FALSE(logger.flush());
    }
    ::close(ends[0]);
    ::close(ends[1]);
}
TEST(Logger, RotatesThroughPersistentDescriptor) {
    const auto path = temporary("-log-fd");
    {
        bmc::Logger logger(path, 200, 2);
        for (unsigned index = 0; index < 8; ++index) {
            logger.action("sensor", "entry-" + std::to_string(index));
        }
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(path.string() + ".1"));
    // 轮转后的分片里必须是完整的 JSON 行，没有因为常驻描述符而被截断。
    std::ifstream input(path.string() + ".1");
    std::string line;
    int lines = 0;
    while (std::getline(input, line)) {
        EXPECT_EQ(line.front(), '{');
        EXPECT_EQ(line.back(), '}');
        ++lines;
    }
    EXPECT_GT(lines, 0);
    for (const auto& suffix : {"", ".1", ".2", ".3"}) {
        std::filesystem::remove(path.string() + suffix);
    }
}
// ============ Gpio / Worker / FaultInjection：硬件路径、任务队列与故障注入 ============
TEST(Gpio, RejectsMalformedPathsBeforeOpeningHardware) {
    auto config = policy();
    config.backend = "gpio";
    for (const auto& path : {"/dev/gpiochip0", "/dev/gpiochip0,-1", "/dev/gpiochip0,1,invalid", "/dev/gpiochip0,4294967296"}) {
        config.path = path;
        EXPECT_THROW(bmc::make_reader(config, bmc::system_io()), std::invalid_argument);
    }
}
TEST(Gpio, MissingDeviceCanRetryAndCloseSafely) {
    auto config = policy();
    config.backend = "gpio";
    config.path = "/definitely/missing/gpiochip,0";
    auto device = bmc::make_device(config, bmc::system_io());
    EXPECT_FALSE(device->open());
    EXPECT_EQ(device->state(), bmc::DeviceState::failed);
    EXPECT_FALSE(device->read_value());
    device->close();
    EXPECT_EQ(device->state(), bmc::DeviceState::closed);
}
TEST(Worker, IsolatesExceptionsAndReportsConcurrentCompletion) {
    bmc::Worker worker(100, 4);
    std::atomic<unsigned> count = 0;
    EXPECT_TRUE(worker.submit([] { throw std::runtime_error("task failure"); }));
    for (unsigned index = 0; index < 50; ++index) EXPECT_TRUE(worker.submit([&count] { ++count; }));
    worker.stop();
    EXPECT_EQ(count.load(), 50u);
    const auto stats = worker.stats();
    EXPECT_EQ(stats.accepted, 51u);
    EXPECT_EQ(stats.completed, 51u);
    EXPECT_EQ(stats.failed, 1u);
    EXPECT_EQ(stats.running, 0u);
    EXPECT_EQ(stats.queued, 0u);
}
// 数值大的优先级先出队，同优先级的任务保持提交顺序。
TEST(Worker, OrdersQueuedTasksByPriorityAndPreservesTies) {
    bmc::Worker worker(8);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    worker.submit([&] { entered.set_value(); released.wait(); });
    entered.get_future().wait();
    std::vector<int> order;
    worker.submit([&] { order.push_back(1); }, 0);
    worker.submit([&] { order.push_back(2); }, 10);
    worker.submit([&] { order.push_back(3); }, 10);
    release.set_value();
    worker.stop();
    EXPECT_EQ(order, (std::vector<int>{2, 3, 1}));
}
TEST(FaultInjection, EventSubscriberExceptionDoesNotStopDelivery) {
    bmc::EventBus bus;
    std::atomic<unsigned> received = 0;
    bus.subscribe(bmc::BusEventType::service, [](const bmc::BusEvent&) {
        throw std::runtime_error("injected subscriber failure");
    });
    bus.subscribe(bmc::BusEventType::service, [&received](const bmc::BusEvent&) { ++received; });
    ASSERT_TRUE(bus.publish({bmc::BusEventType::service, "service", "injected", std::nullopt}));
    bus.stop();
    EXPECT_EQ(received.load(), 1u);
}
// 执行器抛出的异常按一次失败尝试计数，既不上抛也不削减重试次数。
TEST(FaultInjection, RecoveryExecutorExceptionExhaustsAttempts) {
    unsigned attempts = 0;
    bmc::RecoveryPolicyEngine engine([&attempts](const bmc::RecoveryRequest&) -> bool {
        ++attempts;
        throw std::runtime_error("injected recovery failure");
    }, std::chrono::seconds(0), 3);
    const auto result = engine.submit({"rule", "cpu", "increase_fan", 1});
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->accepted);
    EXPECT_FALSE(result->success);
    EXPECT_EQ(result->attempts, 3u);
    EXPECT_EQ(attempts, 3u);
}
// ============ Rules：通配传感器与热重载的状态合并语义 ============
TEST(Rules, WildcardSensorsHaveIndependentState) {
    bmc::FaultRuleEngine engine({{"all", "*", bmc::State::critical, 1, 1, "inspect_device"}});
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
    EXPECT_EQ(engine.evaluate({"fan", bmc::State::normal, bmc::State::critical, 0, "sample", 1}).size(), 1u);
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::critical, bmc::State::normal, 40, "sample", 2}).size(), 1u);
    EXPECT_TRUE(engine.evaluate({"fan", bmc::State::critical, bmc::State::critical, 0, "sample", 2}).empty());
}
TEST(Rules, ReloadKeepsActiveRuleActivated) {
    // 已激活的规则在热重载后必须保持 active，否则每次 reload 都会再触发一次恢复动作。
    bmc::FaultRuleEngine engine({{"r", "cpu", bmc::State::critical, 1, 1, "increase_fan"}});
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
    engine.merge({{"r", "cpu", bmc::State::critical, 1, 1, "increase_fan"}}, {"cpu"});
    // 仍是 critical：已 active，不应再次产生 decision。
    EXPECT_TRUE(engine.evaluate({"cpu", bmc::State::critical, bmc::State::critical, 95, "sample", 2}).empty());
    // 恢复仍然要能正常清除。
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::critical, bmc::State::normal, 40, "sample", 3}).size(), 1u);
}
TEST(Rules, ReloadKeepsPartialConfirmationCount) {
    // confirmations=3：重载前已确认 2 次，重载后第 3 次就应当激活，而不是从头再数。
    bmc::FaultRuleEngine engine({{"r", "cpu", bmc::State::critical, 3, 1, "increase_fan"}});
    EXPECT_TRUE(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).empty());
    EXPECT_TRUE(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).empty());
    engine.merge({{"r", "cpu", bmc::State::critical, 3, 1, "increase_fan"}}, {"cpu"});
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
}
TEST(Rules, ReloadResetsRuleWhoseTriggerChanged) {
    // 触发状态被改写：留下的 active 对应的是旧条件，必须复位并在新条件下重新确认。
    bmc::FaultRuleEngine engine({{"r", "cpu", bmc::State::critical, 1, 1, "increase_fan"}});
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
    // trigger 从 critical 改为 warning。
    engine.merge({{"r", "cpu", bmc::State::warning, 1, 1, "increase_fan"}}, {"cpu"});
    // 新的 bad 判定是 warning（或 critical），因此这里会重新激活一次。
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::warning, 75, "sample", 2}).size(), 1u);
}
TEST(Rules, ReloadDropsStateForRemovedSensor) {
    bmc::FaultRuleEngine engine({{"all", "*", bmc::State::critical, 1, 1, "inspect_device"}});
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
    EXPECT_EQ(engine.evaluate({"fan", bmc::State::normal, bmc::State::critical, 0, "sample", 1}).size(), 1u);
    // fan 被移除：其状态一并丢弃。
    engine.merge({{"all", "*", bmc::State::critical, 1, 1, "inspect_device"}}, {"cpu"});
    // 仍然存在的 cpu 保持 active。
    EXPECT_TRUE(engine.evaluate({"cpu", bmc::State::critical, bmc::State::critical, 95, "sample", 2}).empty());
}
TEST(Rules, ReloadDropsStateForRemovedRule) {
    bmc::FaultRuleEngine engine({{"r", "cpu", bmc::State::critical, 1, 1, "increase_fan"}});
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
    engine.merge({}, {"cpu"});
    // 规则被删除后不再产生任何 decision。
    EXPECT_TRUE(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 2}).empty());
}
TEST(Rules, ReloadRejectsInvalidPolicy) {
    bmc::FaultRuleEngine engine({{"r", "cpu", bmc::State::critical, 1, 1, "increase_fan"}});
    // confirmations 为 0 属于非法策略，必须在改动状态之前被拒绝。
    EXPECT_THROW(bmc::FaultRuleEngine::validate({{"r", "cpu", bmc::State::critical, 0, 1, "increase_fan"}}), std::invalid_argument);
    EXPECT_THROW(bmc::FaultRuleEngine::validate({{"r", "cpu", bmc::State::critical, 1, 1, "power_cycle"}}), std::invalid_argument);
    EXPECT_THROW(engine.merge({{"r", "cpu", bmc::State::critical, 0, 1, "increase_fan"}}, {"cpu"}), std::invalid_argument);
    // 合并失败后原有规则仍然可用。
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
}
// ============ Cli：参数解析与 usage 文案 ============
TEST(Cli, HelpWinsAnywhereAndIgnoresOtherArguments) {
    // --help 出现在任意位置都生效，并且优先于其它（甚至非法的）参数。
    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"--help"},
             {"-h"},
             {"--config", "a.conf", "--help"},
             {"--help", "--config", "a.conf"},
             {"--config", "a.conf", "--bogus", "--help"},
             {"--help", "positional"}}) {
        const auto options = bmc::parse_options(arguments);
        EXPECT_TRUE(options.help) << "arguments: " << arguments.front();
    }
    // 只有真正带 --help 才置位。
    EXPECT_FALSE(bmc::parse_options({"--config", "a.conf"}).help);
}
TEST(Cli, BooleansWorkInAnyPosition) {
    // 旧实现要求布尔开关必须在末尾，否则会被当成前一个选项的取值。
    const auto options = bmc::parse_options({"--enable-actions", "--config", "a.conf", "--ticks", "5"});
    EXPECT_TRUE(options.enable_actions);
    EXPECT_EQ(options.config, "a.conf");
    EXPECT_EQ(options.ticks, 5u);

    const auto trailing = bmc::parse_options({"--config", "a.conf", "--check-config"});
    EXPECT_TRUE(trailing.check_config);
    EXPECT_EQ(trailing.config, "a.conf");

    const auto both = bmc::parse_options({"--check-config", "--enable-actions"});
    EXPECT_TRUE(both.check_config);
    EXPECT_TRUE(both.enable_actions);
}
TEST(Cli, ParsesEveryValueOptionAndDefaults) {
    const auto defaults = bmc::parse_options({});
    EXPECT_EQ(defaults.config, "config/mock.conf");
    EXPECT_EQ(defaults.rules, "config/rules.conf");
    EXPECT_EQ(defaults.sel, "var/sel.db");
    EXPECT_EQ(defaults.log, "var/faults.jsonl");
    EXPECT_EQ(defaults.interval_ms, 1000u);
    EXPECT_EQ(defaults.worker_threads, 2u);
    EXPECT_EQ(defaults.task_capacity, 64u);
    EXPECT_EQ(defaults.ticks, 0u);
    EXPECT_TRUE(defaults.gpio.empty());
    EXPECT_FALSE(defaults.enable_actions);
    EXPECT_FALSE(defaults.check_config);

    const auto options = bmc::parse_options({"--config", "c", "--rules", "r", "--sel", "s", "--log", "l",
        "--gpio", "/dev/gpiochip0", "--interval-ms", "20", "--ticks", "40",
        "--worker-threads", "4", "--task-capacity", "128"});
    EXPECT_EQ(options.config, "c");
    EXPECT_EQ(options.rules, "r");
    EXPECT_EQ(options.sel, "s");
    EXPECT_EQ(options.log, "l");
    EXPECT_EQ(options.gpio, "/dev/gpiochip0");
    EXPECT_EQ(options.interval_ms, 20u);
    EXPECT_EQ(options.ticks, 40u);
    EXPECT_EQ(options.worker_threads, 4u);
    EXPECT_EQ(options.task_capacity, 128u);
}
TEST(Cli, SupportsEqualsSyntax) {
    const auto options = bmc::parse_options({"--config=a.conf", "--interval-ms=20", "--enable-actions"});
    EXPECT_EQ(options.config, "a.conf");
    EXPECT_EQ(options.interval_ms, 20u);
    EXPECT_TRUE(options.enable_actions);
}
TEST(Cli, RejectsMalformedArguments) {
    // 未知选项、缺少取值、非法整数、位置参数都必须报错，而不是静默忽略。
    EXPECT_THROW(bmc::parse_options({"--bogus"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--config"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--interval-ms"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--interval-ms", "0"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--interval-ms", "-1"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--interval-ms", "abc"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--interval-ms", "1000001"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--interval-ms", "20x"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"positional"}), std::invalid_argument);
    // 报错文本要带上出错的选项名，便于定位。
    try {
        bmc::parse_options({"--interval-ms", "abc"});
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("--interval-ms"), std::string::npos);
    }
}
TEST(Cli, UsageMentionsEverySupportedOption) {
    const auto text = bmc::usage();
    for (const auto* option : {"--config", "--rules", "--sel", "--log", "--interval-ms", "--ticks",
                               "--gpio", "--worker-threads", "--task-capacity", "--enable-actions",
                               "--check-config", "--help"}) {
        EXPECT_NE(text.find(option), std::string::npos) << "usage misses " << option;
    }
}

// ============ SocketIo / HttpParser / RateLimiter：外围 TCP 组件 ============
// ---- TCP 组件的可测地基：socket 抽象、HTTP 严格子集解析、限流 ----

// 脚本化的 SocketIo：让连接状态机、解析与限流都能在没有真实端口的情况下测试。
// 可脚本化的部分：
//   - socket/bind/listen/accept/setsockopt/getsockopt/shutdown 各有一个 xxx_results 队列，
//     队列为空时返回默认值；每次调用都记入对应的 xxx_calls，便于断言调用序列与参数。
//   - accept 队列耗尽后返回 -1 并置 EAGAIN，模拟"暂时没有更多连接"；成功时回填回环地址。
//   - recv：incoming 是待读分片队列，每片按自身长度返回，可构造半包与粘包；队列空时置 EAGAIN。
//   - send：默认整包成功并累积到 sent；send_results 非空时逐次消费，用来构造部分写与 EPIPE。
// 明确不模拟：fd 的真实生命周期（close 是非虚的真实 ::close）、真实网络可达性与阻塞超时。
class FakeSocketIo final : public bmc::SocketIo {
public:
    struct Reply {
        int result = 0;        // 返回值；负值表示失败
        int error = 0;         // 失败时的 errno
        std::string payload;   // recv 的载荷；每次调用消费一段
        bool consume_once = false;  // true 时该载荷只返回一次
    };

    int socket(int domain, int type, int protocol) override {
        socket_calls.push_back({domain, type, protocol});
        if (socket_results.empty()) {
            last_error = 0;
            return next_descriptor_++;
        }
        const auto result = socket_results.front();
        socket_results.erase(socket_results.begin());
        last_error = result < 0 ? pending_error : 0;
        return result;
    }
    int bind(int descriptor, const sockaddr* address, socklen_t length) override {
        bind_calls.push_back(descriptor);
        if (!address) {
            bound_names.emplace_back();
        } else {
            bound_names.push_back(bmc::peer_name(address, length));
        }
        return take(&bind_results);
    }
    int listen(int descriptor, int backlog) override {
        listen_calls.push_back({descriptor, backlog});
        return take(&listen_results);
    }
    int accept(int descriptor, sockaddr* address, socklen_t* length) override {
        accept_calls.push_back(descriptor);
        if (accept_results.empty()) {
            last_error = EAGAIN;
            return -1;
        }
        const auto result = accept_results.front();
        accept_results.erase(accept_results.begin());
        if (result < 0) {
            last_error = EAGAIN;
        } else if (address != nullptr && length != nullptr &&
                   *length >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
            sockaddr_in peer {};
            peer.sin_family = AF_INET;
            peer.sin_port = htons(static_cast<std::uint16_t>(40000 + result));
            peer.sin_addr.s_addr = htonl(0x7f000001u);
            std::memcpy(address, &peer, sizeof(peer));
            *length = sizeof(peer);
            last_error = 0;
        } else {
            last_error = 0;
        }
        return result;
    }
    int setsockopt(int descriptor, int level, int name, const void* value, socklen_t length) override {
        setsockopt_calls.push_back({descriptor, level, name});
        (void)value;
        (void)length;
        return take(&setsockopt_results);
    }
    int getsockopt(int descriptor, int level, int name, void* value, socklen_t* length) override {
        (void)value;
        (void)length;
        (void)descriptor;
        (void)level;
        (void)name;
        return take(&getsockopt_results);
    }
    ssize_t recv(int descriptor, void* buffer, std::size_t count, int flags) override {
        (void)flags;
        recv_calls.push_back(descriptor);
        if (incoming.empty()) {
            last_error = EAGAIN;
            return -1;
        }
        // 支持显式分片：每个元素按自身长度返回，便于构造半包与粘包。
        auto chunk = incoming.front();
        incoming.erase(incoming.begin());
        const auto size = std::min(count, chunk.size());
        std::memcpy(buffer, chunk.data(), size);
        const bool partial = size < chunk.size();
        if (partial) {
            incoming.insert(incoming.begin(), chunk.substr(size));
            last_error = 0;
            return static_cast<ssize_t>(size);
        }
        last_error = 0;
        return static_cast<ssize_t>(size);
    }
    ssize_t send(int descriptor, const void* buffer, std::size_t count, int flags) override {
        (void)flags;
        send_calls.push_back(descriptor);
        if (!send_results.empty()) {
            const auto result = send_results.front();
            send_results.erase(send_results.begin());
            last_error = result < 0 ? EPIPE : 0;
            return result;
        }
        sent.append(static_cast<const char*>(buffer), count);
        last_error = 0;
        return static_cast<ssize_t>(count);
    }
    int shutdown(int descriptor, int how) override {
        shutdown_calls.push_back({descriptor, how});
        return take(&shutdown_results);
    }

    // 队列化返回值；空队列时取默认值。
    std::vector<int> socket_results;
    std::vector<int> bind_results;
    std::vector<int> listen_results;
    std::vector<int> accept_results;
    std::vector<int> setsockopt_results;
    std::vector<int> getsockopt_results;
    std::vector<int> shutdown_results;
    std::vector<ssize_t> send_results;
    std::deque<std::string> incoming;
    int pending_error = EINVAL;

    std::vector<std::array<int, 3>> socket_calls;
    std::vector<int> bind_calls;
    std::vector<std::string> bound_names;
    std::vector<std::array<int, 2>> listen_calls;
    std::vector<int> accept_calls;
    std::vector<std::array<int, 3>> setsockopt_calls;
    std::vector<int> recv_calls;
    std::vector<int> send_calls;
    std::vector<std::array<int, 2>> shutdown_calls;
    std::string sent;

private:
    int take(std::vector<int>* queue) {
        if (queue->empty()) {
            last_error = 0;
            return 0;
        }
        const auto result = queue->front();
        queue->erase(queue->begin());
        last_error = result < 0 ? pending_error : 0;
        return result;
    }
    int next_descriptor_ = 100;
};

TEST(SocketIo, PosixImplementationMatchesSystemCalls) {
    // 用一个真实但立即关闭的 UDP 套接字验证直通实现，不依赖网络可用性。
    bmc::PosixSocketIo io;
    const int descriptor = io.socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(descriptor, 0) << "socket failed: " << io.last_error;
    // bind 到回环的临时端口。
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(io.bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    // 不支持的 option 应返回 -1 并留下 errno。
    int option = 0;
    socklen_t length = sizeof(option);
    EXPECT_EQ(io.getsockopt(descriptor, SOL_SOCKET, -1, &option, &length), -1);
    EXPECT_NE(io.last_error, 0);
    EXPECT_EQ(io.close(descriptor), 0);
}

TEST(SocketIo, PeerNameFormatsIpv4AndRejectsOthers) {
    sockaddr_in ipv4 {};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(8080);
    ipv4.sin_addr.s_addr = htonl(0x0a000001u);  // 10.0.0.1
    EXPECT_EQ(bmc::peer_name(reinterpret_cast<const sockaddr*>(&ipv4), sizeof(ipv4)), "10.0.0.1:8080");
    // 长度不足或族不支持时返回空串，而不是读越界。
    // 长度不足以容纳 sockaddr_in 时返回空串，而不是读越界。
    EXPECT_EQ(bmc::peer_name(reinterpret_cast<const sockaddr*>(&ipv4), sizeof(ipv4) - 1), "");
    EXPECT_EQ(bmc::peer_name(nullptr, 0), "");
    sockaddr_un unsupported {};
    unsupported.sun_family = AF_UNIX;
    EXPECT_EQ(bmc::peer_name(reinterpret_cast<const sockaddr*>(&unsupported), sizeof(unsupported)), "");
}

TEST(SocketIo, FakeDrivesAcceptRecvAndSendWithoutRealPorts) {
    FakeSocketIo io;
    const int listener = io.socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_EQ(listener, 100);
    EXPECT_EQ(io.socket_calls.size(), 1u);
    io.accept_results = {7, -1};  // 先接受一个连接，再返回"无更多连接"
    sockaddr_storage peer {};
    socklen_t length = sizeof(peer);
    EXPECT_EQ(io.accept(listener, reinterpret_cast<sockaddr*>(&peer), &length), 7);
    EXPECT_EQ(io.accept(listener, nullptr, nullptr), -1);
    EXPECT_EQ(io.last_error, EAGAIN);    // recv 支持分片：先给一个半包，再给剩下部分。
    io.incoming = {"GET /a HTTP/1.1\r\nHost: x\r\n", "\r\n"};
    char buffer[64] = {};
    EXPECT_GT(io.recv(7, buffer, sizeof(buffer), 0), 0);
    EXPECT_GT(io.recv(7, buffer, sizeof(buffer), 0), 0);
    EXPECT_EQ(io.recv(7, buffer, sizeof(buffer), 0), -1);
    // send 默认整包成功，也可脚本化为部分写入或 EPIPE。
    EXPECT_EQ(io.send(7, "abc", 3, 0), 3);
    EXPECT_EQ(io.sent, "abc");
    io.send_results = {1, -1};
    EXPECT_EQ(io.send(7, "abc", 3, 0), 1);
    EXPECT_EQ(io.send(7, "abc", 3, 0), -1);
    EXPECT_EQ(io.last_error, EPIPE);
    EXPECT_EQ(io.shutdown(7, SHUT_RDWR), 0);
    ASSERT_EQ(io.shutdown_calls.size(), 1u);
    EXPECT_EQ(io.shutdown_calls.front()[0], 7);
    EXPECT_EQ(io.shutdown_calls.front()[1], SHUT_RDWR);
    // close() is the real ::close (non-virtual), so the success path needs a real
    // descriptor while the errno path needs an unopened one.
    int pipe_ends[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipe_ends), 0);
    EXPECT_EQ(io.close(pipe_ends[0]), 0);
    EXPECT_EQ(io.last_error, 0);
    EXPECT_EQ(io.close(pipe_ends[1]), 0);
    // An unopened fake descriptor really does fail; the errno is kept for diagnostics.
    EXPECT_EQ(io.close(7), -1);
    EXPECT_EQ(io.last_error, EBADF);
}

namespace {
bmc::http::ParseResult feed_all(bmc::http::Parser& parser, const std::string& text) {
    // 逐字节喂入，顺带验证增量解析在任意分片边界都能工作。
    bmc::http::ParseResult result = bmc::http::ParseResult::incomplete;
    for (const char character : text) {
        result = parser.feed(&character, 1);
        if (result != bmc::http::ParseResult::incomplete) {
            return result;
        }
    }
    return result;
}
}

TEST(HttpParser, ParsesGetWithHeadersAndIsCaseInsensitive) {
    bmc::http::Parser parser;
    const auto result = feed_all(parser,
        "GET /redfish/v1/ HTTP/1.1\r\nHost: bmc.local:8000\r\nAccept: application/json\r\n\r\n");
    ASSERT_EQ(result, bmc::http::ParseResult::complete);
    EXPECT_EQ(parser.request().method, "GET");
    EXPECT_EQ(parser.request().target, "/redfish/v1/");
    EXPECT_EQ(parser.request().version, "HTTP/1.1");
    EXPECT_TRUE(parser.request().body.empty());
    EXPECT_EQ(parser.request().header("host"), "bmc.local:8000");
    EXPECT_EQ(parser.request().header("HOST"), "bmc.local:8000");
    EXPECT_EQ(parser.request().header("ACCEPT"), "application/json");
    EXPECT_TRUE(parser.request().has_header("Accept"));
    EXPECT_FALSE(parser.request().has_header("Authorization"));
    EXPECT_EQ(parser.request().header("Authorization"), "");
}

TEST(HttpParser, ParsesPostBodyByContentLength) {
    bmc::http::Parser parser;
    const auto result = feed_all(parser,
        "POST /v1/actions/fan HTTP/1.1\r\nHost: bmc\r\nContent-Length: 26\r\n\r\n"
        "{\"sensor\":\"cpu\",\"act\":\"x\"}");
    ASSERT_EQ(result, bmc::http::ParseResult::complete);
    EXPECT_EQ(parser.request().method, "POST");
    EXPECT_EQ(parser.request().target, "/v1/actions/fan");
    EXPECT_EQ(parser.request().body, "{\"sensor\":\"cpu\",\"act\":\"x\"}");
    EXPECT_EQ(parser.request().body.size(), 26u);
}

TEST(HttpParser, HandlesBodySplitAcrossArbitraryChunks) {
    // 粘包 + 半包：一次喂入两条请求的前缀，再补全。
    bmc::http::Parser parser;
    EXPECT_EQ(parser.feed("POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nab"),
              bmc::http::ParseResult::incomplete);
    EXPECT_EQ(parser.buffered_bytes(), 2u);
    EXPECT_EQ(parser.feed("cde"), bmc::http::ParseResult::complete);
    EXPECT_EQ(parser.request().body, "abcde");
    // 同一连接上的下一条请求：feed 返回 complete 后解析器自动丢弃多余字节，
    // 因此直接继续 feed 即可，不会误判为 incomplete。
    EXPECT_EQ(parser.feed("GET /b HTTP/1.1\r\n\r\n"), bmc::http::ParseResult::complete);
    EXPECT_EQ(parser.request().target, "/b");
    EXPECT_EQ(parser.buffered_bytes(), 0u);
    parser.reset();
    EXPECT_EQ(parser.buffered_bytes(), 0u);
}

TEST(HttpParser, RejectsChunkedAndOtherUnsupportedForms) {
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "POST /a HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
                  bmc::http::ParseResult::unsupported);
    }
    // 方法必须是 token，"GET\t/" 之类被拒。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "GE T /a HTTP/1.1\r\n\r\n"), bmc::http::ParseResult::malformed);
    }
    // 版本必须是 1.0/1.1。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "GET /a HTTP/2\r\n\r\n"), bmc::http::ParseResult::malformed);
    }
    // 头部缺少冒号。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "GET /a HTTP/1.1\r\nBroken\r\n\r\n"), bmc::http::ParseResult::malformed);
    }
    // 头部名含非法字符。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "GET /a HTTP/1.1\r\nBad Header: x\r\n\r\n"), bmc::http::ParseResult::malformed);
    }
    // Content-Length 非数字或溢出。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "POST /a HTTP/1.1\r\nContent-Length: 5x\r\n\r\n"),
                  bmc::http::ParseResult::malformed);
    }
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "POST /a HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n"),
                  bmc::http::ParseResult::malformed);
    }
    // 目标含控制字符（请求行注入尝试）。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "GET /a\x01b HTTP/1.1\r\n\r\n"), bmc::http::ParseResult::malformed);
    }
}

TEST(HttpParser, EnforcesSizeLimits) {
    // 请求行超限。
    {
        bmc::http::Parser parser;
        const std::string huge = "GET /" + std::string(bmc::http::kMaxRequestLine + 10, 'a') + " HTTP/1.1\r\n\r\n";
        EXPECT_EQ(parser.feed(huge), bmc::http::ParseResult::too_large);
    }
    // 单个头部行超限。
    {
        bmc::http::Parser parser;
        const std::string huge = "GET /a HTTP/1.1\r\nX: " + std::string(bmc::http::kMaxHeaderLine + 10, 'b') + "\r\n\r\n";
        EXPECT_EQ(parser.feed(huge), bmc::http::ParseResult::too_large);
    }
    // 头部总量超限：用许多合法小头部堆到上限之上。
    {
        bmc::http::Parser parser;
        std::string request = "GET /a HTTP/1.1\r\n";
        while (request.size() < bmc::http::kMaxHeaderTotal + 512) {
            request += "X-Pad: 0123456789\r\n";
        }
        request += "\r\n";
        EXPECT_EQ(parser.feed(request), bmc::http::ParseResult::too_large);
    }
    // 头部条数超限。
    {
        bmc::http::Parser parser;
        std::string request = "GET /a HTTP/1.1\r\n";
        for (std::size_t index = 0; index < bmc::http::kMaxHeaders + 1; ++index) {
            request += "X: 1\r\n";
        }
        request += "\r\n";
        EXPECT_EQ(parser.feed(request), bmc::http::ParseResult::too_large);
    }
    // 声明超长请求体：头部解析完即拒绝，不需要真的收到那么多字节。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(feed_all(parser, "POST /a HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n"),
                  bmc::http::ParseResult::too_large);
    }
    // 错误状态是粘滞的，直到 reset()。
    {
        bmc::http::Parser parser;
        EXPECT_EQ(parser.feed(std::string("GET /") + std::string(bmc::http::kMaxRequestLine + 4, 'a')),
                  bmc::http::ParseResult::too_large);
        EXPECT_EQ(parser.feed("GET /a HTTP/1.1\r\n\r\n"), bmc::http::ParseResult::too_large);
        parser.reset();
        EXPECT_EQ(feed_all(parser, "GET /a HTTP/1.1\r\n\r\n"), bmc::http::ParseResult::complete);
    }
}

TEST(HttpParser, IncompleteInputNeverReportsComplete) {
    bmc::http::Parser parser;
    // 一个只发一半头部就停下的慢客户端：解析器只能一直说"还要更多"。
    EXPECT_EQ(parser.feed("GET /redfish/v1/ HTTP/1.1\r\nHost: bmc"), bmc::http::ParseResult::incomplete);
    EXPECT_EQ(parser.feed("\r\nContent-Length: 4\r\n"), bmc::http::ParseResult::incomplete);
    EXPECT_EQ(parser.buffered_bytes(), 0u);
    EXPECT_EQ(parser.feed(""), bmc::http::ParseResult::incomplete);
    EXPECT_EQ(parser.feed(nullptr, 0), bmc::http::ParseResult::incomplete);
}

TEST(RateLimiter, EnforcesCapacityAndRefillsOverTime) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point{} + std::chrono::seconds(1);
    bmc::http::RateLimiter limiter(3, 1.0, 16);  // 容量 3，每秒回填 1 个

    EXPECT_TRUE(limiter.allow("10.0.0.1", start));
    EXPECT_TRUE(limiter.allow("10.0.0.1", start));
    EXPECT_TRUE(limiter.allow("10.0.0.1", start));
    EXPECT_FALSE(limiter.allow("10.0.0.1", start)) << "capacity exhausted";
    // 每个键独立计数。
    EXPECT_TRUE(limiter.allow("10.0.0.2", start));
    // 1 秒后回填 1 个令牌，恰好放行一次。
    const auto later = start + std::chrono::seconds(1);
    EXPECT_TRUE(limiter.allow("10.0.0.1", later));
    EXPECT_FALSE(limiter.allow("10.0.0.1", later));
    // 长时间空闲不会超过容量。
    const auto much_later = later + std::chrono::hours(1);
    EXPECT_NEAR(limiter.tokens("10.0.0.1", much_later), 3.0, 1e-9);
    EXPECT_TRUE(limiter.allow("10.0.0.1", much_later));
    EXPECT_TRUE(limiter.allow("10.0.0.1", much_later));
    EXPECT_TRUE(limiter.allow("10.0.0.1", much_later));
    EXPECT_FALSE(limiter.allow("10.0.0.1", much_later));
}

TEST(RateLimiter, BoundsTrackedKeysAndEvictsOldest) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::time_point{} + std::chrono::seconds(1);
    bmc::http::RateLimiter limiter(2, 0.0, 3);
    EXPECT_TRUE(limiter.allow("a", now));
    EXPECT_TRUE(limiter.allow("b", now + std::chrono::seconds(1)));
    EXPECT_TRUE(limiter.allow("c", now + std::chrono::seconds(2)));
    EXPECT_EQ(limiter.tracked_keys(), 3u);
    // 表满后仍接纳新来源，并保留最近访问的来源。
    EXPECT_TRUE(limiter.allow("d", now + std::chrono::seconds(3)));
    EXPECT_EQ(limiter.tracked_keys(), 3u);
    EXPECT_NEAR(limiter.tokens("a", now + std::chrono::seconds(3)), 2.0, 1e-9);
    EXPECT_NEAR(limiter.tokens("b", now + std::chrono::seconds(3)), 1.0, 1e-9);
    for (unsigned index = 0; index < 2000; ++index)
        EXPECT_TRUE(limiter.allow("source-" + std::to_string(index), now + std::chrono::seconds(4 + index)));
    EXPECT_EQ(limiter.tracked_keys(), 3u);
    EXPECT_TRUE(limiter.allow("admin", now + std::chrono::seconds(3000)));
    limiter.clear();
    EXPECT_EQ(limiter.tracked_keys(), 0u);
    EXPECT_TRUE(limiter.allow("d", now));
}

TEST(RateLimiter, RejectsInvalidConfiguration) {
    EXPECT_THROW(bmc::http::RateLimiter(0, 1.0, 4), std::invalid_argument);
    EXPECT_THROW(bmc::http::RateLimiter(1, -1.0, 4), std::invalid_argument);
    EXPECT_THROW(bmc::http::RateLimiter(1, 1.0, 0), std::invalid_argument);
}

// ============ ReadOnlyService：与 Python 参考实现逐字对拍 ============
// ---- Read-only service: byte-for-byte parity with tools/bmc_manage.py ----
// The golden vectors come from tools/gen_service_vectors.py, which runs the reference
// Python implementation, so these tests fail if the C++ output drifts even by one byte.

// 锁定 Python json.dumps 的排版细节：分隔符、整数值浮点、转义与键序，差一个字节即失败。
TEST(ReadOnlyService, JsonWriterMatchesPythonDumpsFormatting) {
    // Separators are ", " and ": ". 整数值的浮点省略小数部分：这与生成的向量一致，
    // 因为生成器把整数记录原样交给 json.dumps（json.dumps(95) -> "95"），
    // 只在 C++ 字面量那一侧用 float() 换算。
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(95.0)), "95");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(41.5)), "41.5");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(0.125)), "0.125");
    // 指数 >= 16 保持科学计数法，1e15 仍展开为整数，两者都与 json.dumps 相同。
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(1e15)), "1000000000000000");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(1e16)), "1e+16");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(static_cast<std::int64_t>(1700000000000))),
              "1700000000000");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::null()), "null");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(false)), "false");
    // Quotes and backslashes are escaped; non-ASCII is passed through (ensure_ascii=False).
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(std::string("odd\"sensor"))), "\"odd\\\"sensor\"");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(std::string("a\\b"))), "\"a\\\\b\"");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(std::string("\xe6\xb8\xa9\xe5\xba\xa6"))), "\"\xe6\xb8\xa9\xe5\xba\xa6\"");
    EXPECT_EQ(bmc::JsonWriter::dump(bmc::JsonWriter::Value::of(std::string("a\nb"))), "\"a\\nb\"");
    // Member insertion order is preserved.
    const auto document = bmc::JsonWriter::Value::object()
        .set("b", bmc::JsonWriter::Value::of(static_cast<std::int64_t>(1)))
        .set("a", bmc::JsonWriter::Value::of(std::string("x")));
    EXPECT_EQ(bmc::JsonWriter::dump(document), "{\"b\": 1, \"a\": \"x\"}");
    // NaN/Infinity are rejected rather than written as invalid JSON.
    EXPECT_THROW(bmc::JsonWriter::number(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    EXPECT_THROW(bmc::JsonWriter::number(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST(ReadOnlyService, ResourcesMatchTheReferenceImplementationByteForByte) {
    ASSERT_FALSE(kResourceVectors.empty());
    for (const auto& [path, expected] : kResourceVectors) {
        const auto body = bmc::readonly::resource(path, kVectorEntries);
        if (expected.empty()) {
            EXPECT_FALSE(body.has_value()) << "expected 404 for " << path;
            continue;
        }
        ASSERT_TRUE(body.has_value()) << "expected a resource for " << path;
        EXPECT_EQ(bmc::JsonWriter::dump(*body), expected) << "mismatch for " << path;
    }
}

TEST(ReadOnlyService, MetricsMatchTheReferenceImplementationByteForByte) {
    EXPECT_EQ(bmc::readonly::metrics(kVectorEntries), kMetricsVector);
}

TEST(ReadOnlyService, HandleDispensesStatusContentTypeAndBody) {
    const auto metrics = bmc::readonly::handle("/metrics", kVectorEntries);
    EXPECT_EQ(metrics.status, 200);
    EXPECT_EQ(metrics.content_type, "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_EQ(metrics.body, kMetricsVector);

    const auto entries = bmc::readonly::handle("/redfish/v1/Managers/BMC/LogServices/SEL/Entries", kVectorEntries);
    EXPECT_EQ(entries.status, 200);
    EXPECT_EQ(entries.content_type, "application/json; charset=utf-8");
    ASSERT_GE(kResourceVectors.size(), 6u);
    EXPECT_EQ(entries.body, kResourceVectors[5].second);

    // Unknown paths return the same error body as the reference implementation.
    const auto missing = bmc::readonly::handle("/nope", kVectorEntries);
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.body,
              "{\"error\": {\"code\": \"ResourceNotFound\", \"message\": \"Unknown resource\"}}");

    // A query string is ignored, matching urlsplit(path).path.
    const auto with_query = bmc::readonly::handle("/redfish/v1/?x=1", kVectorEntries);
    EXPECT_EQ(with_query.status, 200);
    EXPECT_EQ(with_query.body, kResourceVectors[0].second);

    // /healthz is a local probe and is not a Redfish resource.
    const auto health = bmc::readonly::handle("/healthz", {});
    EXPECT_EQ(health.status, 200);
    EXPECT_EQ(health.body, "ok\n");
}

TEST(ReadOnlyService, RenderedResponseCarriesLengthAndNoStore) {
    const auto response = bmc::readonly::handle("/healthz", {});
    const auto text = response.render();
    EXPECT_EQ(text.rfind("HTTP/1.1 200 OK\r\n", 0), 0u);
    EXPECT_NE(text.find("Content-Type: text/plain; charset=utf-8\r\n"), std::string::npos);
    EXPECT_NE(text.find("Content-Length: 3\r\n"), std::string::npos);
    EXPECT_NE(text.find("Cache-Control: no-store\r\n"), std::string::npos);
    // Content-Length must equal the real body length or the client hangs.
    const auto separator = text.find("\r\n\r\n");
    ASSERT_NE(separator, std::string::npos);
    EXPECT_EQ(text.size() - (separator + 4), response.body.size());
    EXPECT_EQ(bmc::readonly::handle("/nope", {}).render().find("HTTP/1.1 404 Not Found\r\n"), 0u);
}

TEST(ReadOnlyService, SelParsingRoundTripsThroughTheFileFormat) {
    const auto path = temporary("-service.sel");
    std::filesystem::remove(path);
    {
        // Write through the real SelStore so the parser sees production encoding.
        bmc::SelStore store(path, 1000);
        for (const auto& entry : kVectorEntries) {
            store.append(entry.sensor, entry.state, entry.message, entry.value);
        }
        ASSERT_TRUE(store.flush());
    }
    const auto parsed = bmc::read_sel_entries(path.string());
    ASSERT_EQ(parsed.size(), kVectorEntries.size());
    for (std::size_t index = 0; index < parsed.size(); ++index) {
        EXPECT_EQ(parsed[index].sensor, kVectorEntries[index].sensor);
        EXPECT_EQ(parsed[index].state, kVectorEntries[index].state);
        EXPECT_EQ(parsed[index].message, kVectorEntries[index].message);
        EXPECT_EQ(parsed[index].value.has_value(), kVectorEntries[index].value.has_value());
        if (parsed[index].value && kVectorEntries[index].value) {
            EXPECT_NEAR(*parsed[index].value, *kVectorEntries[index].value, 1e-12);
        }
    }
    for (std::size_t index = 1; index < parsed.size(); ++index) {
        EXPECT_GT(parsed[index].id, parsed[index - 1].id);
    }
    std::filesystem::remove(path);
}

TEST(ReadOnlyService, SelParsingSkipsUnreadableLinesAndBoundsTheWindow) {
    const auto path = temporary("-service-broken.sel");
    std::filesystem::remove(path);
    {
        std::ofstream output(path);
        output << "1 1700000000000 \"cpu\" \"normal\" \"ok\" 41.5\n";
        output << "not a valid record\n";
        output << "2 1700000001000 \"cpu\" \"warning\"\n";
        output << "x 1700000002000 \"cpu\" \"normal\" \"ok\" 1\n";
        output << "3 1700000003000 \"cpu\" \"normal\" \"ok\" null\n";
        output << "4 1700000004000 \"cpu\" \"normal\" \"ok\" nope\n";
        output << "\n";
    }
    const auto parsed = bmc::read_sel_entries(path.string());
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0].sensor, "cpu");
    EXPECT_TRUE(parsed[0].value.has_value());
    EXPECT_NEAR(*parsed[0].value, 41.5, 1e-12);
    EXPECT_FALSE(parsed[1].value.has_value());
    // max_records keeps only the most recent records.
    const auto windowed = bmc::read_sel_entries(path.string(), 1);
    ASSERT_EQ(windowed.size(), 1u);
    EXPECT_EQ(windowed.front().sensor, "cpu");
    EXPECT_FALSE(windowed.front().value.has_value());
    // A missing file is not an error.
    EXPECT_TRUE(bmc::read_sel_entries(
        (std::filesystem::temp_directory_path() / "definitely-missing.sel").string()).empty());
    std::filesystem::remove(path);
}
}
