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
}
