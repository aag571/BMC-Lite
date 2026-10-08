#include "bmc/core.hpp"
#include <unistd.h>
#include <utility>

namespace bmc {
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
