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
std::string name(State state) {
    switch (state) {
    case State::normal: return "normal";
    case State::warning: return "warning";
    case State::critical: return "critical";
    case State::unavailable: return "unavailable";
    }
    throw std::invalid_argument("invalid state");
}
std::string escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec;
            } else {
                output << character;
            }
        }
    }
    return output.str();
}
}
