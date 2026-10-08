#include "bmc/core.hpp"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace bmc {
namespace {
[[noreturn]] void system_failure(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
// 只为"写失败也要关掉描述符"提供作用域退出，不占用 Fd（Fd 的析构走真实 ::close）。
class DescriptorGuard {
public:
    DescriptorGuard(LinuxIo& io, int descriptor) noexcept : io_(io), descriptor_(descriptor) {}
    DescriptorGuard(const DescriptorGuard&) = delete;
    DescriptorGuard& operator=(const DescriptorGuard&) = delete;
    ~DescriptorGuard() { io_.close(descriptor_); }
private:
    LinuxIo& io_;
    int descriptor_;
};
}
void write_pwm(const std::string& path, unsigned value, LinuxIo& io) {
    if (value > 255) {
        throw std::invalid_argument("PWM value out of range");
    }
    const int descriptor = io.open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        system_failure("open PWM");
    }
    const DescriptorGuard guard(io, descriptor);
    const std::string text = std::to_string(value) + "\n";
    const auto count = io.write(descriptor, text.data(), text.size());
    if (count < 0) {
        system_failure("write PWM");
    }
    if (static_cast<std::size_t>(count) != text.size()) {
        throw std::runtime_error("short PWM write");
    }
}
}
