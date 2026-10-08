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
Logger::Logger(std::filesystem::path path, std::uintmax_t limit, unsigned keep, bool sync, std::size_t batch_bytes)
    : path_(std::move(path)), limit_(limit), keep_(keep), policy_{batch_bytes, sync, 1u << 20} {
    if (limit == 0 || keep == 0 || keep > 100) {
        throw std::invalid_argument("invalid rotation policy");
    }
    if (batch_bytes == 0) {
        throw std::invalid_argument("zero log batch size");
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    open();
}
Logger::~Logger() {
    std::lock_guard lock(mutex_);
    // 尽力把缓冲区落盘；失败也不能在这里抛异常（析构期间）。
    drain(false);
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
bool Logger::drain(bool sync) {
    if (descriptor_ < 0) {
        return pending_.empty();
    }
    if (pending_.empty()) {
        return true;
    }
    try {
        write_all(descriptor_, pending_);
    } catch (const std::exception&) {
        // 写入失败时保留缓冲区等待重试，并计数。
        ++write_failures_;
        return false;
    }
    pending_.clear();
    if (sync && ::fdatasync(descriptor_) < 0) {
        // 同步失败不表示写入失败，但仍记录失败次数以便观测。
        ++write_failures_;
    }
    return true;
}
bool Logger::drop_oldest_excess() {
    if (pending_.size() <= policy_.pending_cap) {
        return false;
    }
    const auto excess = pending_.size() - policy_.pending_cap;
    pending_.erase(0, excess);
    dropped_bytes_ += excess;
    // 丢弃未落盘内容同样属于"日志不完整"，计入失败次数以便上报。
    ++write_failures_;
    return true;
}
bool Logger::rotate(std::size_t incoming) {
    std::error_code status;
    const auto size = std::filesystem::file_size(path_, status);
    if (status) {
        // 无法得知文件大小（管道、设备节点等）时不轮转，但仍然必须尝试写入，
        // 否则这些目标上的写入失败将永远不被发现。
        return true;
    }
    // 只有"当前文件 + 尚未落盘的缓冲区 + 新记录"确实要超过上限时才轮转。
    // 仅仅为了检查是否轮转而无条件 drain，会让缓冲区永远是空的，批量合并就完全失效了。
    if (size + pending_.size() + incoming <= limit_) {
        return true;
    }
    // 轮转前必须先把未落盘缓冲区写进当前分片，否则这批行会跟着文件被改名而丢失。
    if (!drain(false)) {
        // 连写入都失败：保持当前文件与缓冲区不动，避免把未落盘内容一起丢掉。
        return false;
    }
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
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
    return true;
}
void Logger::append(const std::string& line) {
    std::lock_guard lock(mutex_);
    const auto record = line + "\n";
    // rotate 只在确实要超限时才落盘改名；失败时保留缓冲区并已在 drain 中计数。
    const bool rotated = rotate(record.size());
    pending_ += record;
    // 非普通文件无法预知大小（file_size 报错），此时跳过轮转判断但必须尝试写入，
    // 否则这些目标上的写入失败永远不会被观测到。
    std::error_code status;
    static_cast<void>(std::filesystem::file_size(path_, status));
    if (!rotated || policy_.sync || status || pending_.size() >= policy_.batch_bytes) {
        drain(policy_.sync);
    }
    drop_oldest_excess();
}
bool Logger::flush() {
    std::lock_guard lock(mutex_);
    drain(policy_.sync);
    return pending_.empty();
}
std::size_t Logger::pending_bytes() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}
std::uint64_t Logger::write_failures() const {
    std::lock_guard lock(mutex_);
    return write_failures_;
}
std::uint64_t Logger::dropped_bytes() const {
    std::lock_guard lock(mutex_);
    return dropped_bytes_;
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
