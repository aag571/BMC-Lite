#include "bmc/core.hpp"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace bmc {
namespace {
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(milliseconds);
}
void write_all(int descriptor, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto written = ::write(descriptor, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "write log");
        }
        if (written == 0) {
            throw std::runtime_error("short log write");
        }
        offset += static_cast<std::size_t>(written);
    }
}
}
Logger::Logger(std::filesystem::path path, std::uintmax_t limit, unsigned keep)
    : path_(std::move(path)), limit_(limit), keep_(keep) {
    if (limit == 0 || keep == 0 || keep > 100) {
        throw std::invalid_argument("invalid rotation policy");
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    open();
}
Logger::~Logger() {
    std::lock_guard lock(mutex_);
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}
void Logger::open() {
    descriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open log");
    }
}
void Logger::rotate(std::size_t incoming) {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    std::error_code status;
    const auto size = std::filesystem::file_size(path_, status);
    if (status || size + incoming <= limit_) {
        // 未超限（或文件尚不存在）时无需轮转，直接重新打开继续追加。
        open();
        return;
    }
    for (unsigned index = keep_; index > 0; --index) {
        const auto destination = std::filesystem::path(path_.string() + "." + std::to_string(index));
        const auto source = index == 1 ? path_ : std::filesystem::path(path_.string() + "." + std::to_string(index - 1));
        if (std::filesystem::exists(destination)) {
            std::filesystem::remove(destination);
        }
        if (std::filesystem::exists(source)) {
            std::filesystem::rename(source, destination);
        }
    }
    open();
}
void Logger::append(const std::string& line) {
    std::lock_guard lock(mutex_);
    rotate(line.size() + 1);
    if (descriptor_ < 0) {
        throw std::runtime_error("log descriptor is closed");
    }
    write_all(descriptor_, line + "\n");
}
void Logger::write(const Event& event) {
    std::ostringstream output;
    output << std::setprecision(17) << "{\"time_ms\":" << timestamp()
           << ",\"sensor\":\"" << escape(event.id)
           << "\",\"before\":\"" << name(event.before)
           << "\",\"state\":\"" << name(event.after)
           << "\",\"value\":";
    if (event.value && std::isfinite(*event.value)) {
        output << *event.value;
    } else {
        output << "null";
    }
    output << ",\"reason\":\"" << escape(event.reason) << "\",\"sequence\":" << event.sequence << '}';
    append(output.str());
}
void Logger::action(const std::string& id, const std::string& result) {
    append("{\"time_ms\":" + timestamp() + ",\"sensor\":\"" + escape(id) + "\",\"action\":\"" + escape(result) + "\"}");
}
}
