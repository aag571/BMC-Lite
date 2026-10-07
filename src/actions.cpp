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
