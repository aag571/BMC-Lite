#include "bmc/core.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace bmc {
namespace {
double number(const std::string& token) {
    std::size_t consumed = 0;
    const double value = std::stod(token, &consumed);
    if (consumed != token.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid number: " + token);
    }
    return value;
}
std::string encode(const SelRecord& record) {
    std::ostringstream output;
    output << record.id << ' ' << record.time_ms << ' ' << std::quoted(record.source) << ' '
           << std::quoted(record.state) << ' ' << std::quoted(record.message) << ' ';
    if (record.value && std::isfinite(*record.value)) {
        output << std::setprecision(17) << *record.value;
    } else {
        output << "null";
    }
    output << '\n';
    return output.str();
}
bool decode(const std::string& line, SelRecord& record) {
    std::istringstream fields(line);
    std::string value;
    if (!(fields >> record.id >> record.time_ms >> std::quoted(record.source) >> std::quoted(record.state) >>
          std::quoted(record.message) >> value)) {
        return false;
    }
    if (value != "null") {
        try {
            record.value = number(value);
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}
// 与 ofstream 不同，这里用 O_APPEND 的常驻描述符；日志与 SEL 都不允许其他写者加入。
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

void SelStore::write_probe(const std::filesystem::path& path) {
    const int probe = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (probe < 0 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "open sel");
    }
    if (probe >= 0) {
        ::close(probe);
    }
}

SelStore::SelStore(std::filesystem::path path, std::size_t max_records, Truncate policy)
    : path_(std::move(path)), max_records_(max_records), policy_(policy) {
    if (max_records_ == 0) {
        throw std::invalid_argument("zero SEL capacity");
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    std::lock_guard lock(mutex_);
    open();
    replay();
}

SelStore::~SelStore() {
    std::lock_guard lock(mutex_);
    drain(true);
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

void SelStore::open() {
    // 已有文件（例如 /dev/null、已配置好的日志路径）不追加 O_CREAT，
    // 避免在只允许打开既有设备的路径上拿到 EACCES。
    std::error_code status;
    const bool exists = std::filesystem::exists(path_, status);
    const int flags = O_RDWR | O_APPEND | O_CLOEXEC | (exists ? 0 : O_CREAT);
    descriptor_ = ::open(path_.c_str(), flags, 0644);
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open sel");
    }
    std::error_code type_status;
    syncable_ = std::filesystem::is_regular_file(path_, type_status) && !type_status;
}

void SelStore::replay() {
    if (descriptor_ < 0) {
        return;
    }
    // 某些路径（例如 /dev/null 或其上的日志设备）不支持查询大小，此时无可重放内容。
    std::error_code status;
    const auto file_size = std::filesystem::file_size(path_, status);
    if (status) {
        return;
    }
    const auto size = file_size;
    if (size == 0) {
        return;
    }
    std::vector<char> content(static_cast<std::size_t>(size));
    ssize_t read_total = 0;
    while (read_total < static_cast<ssize_t>(content.size())) {
        const auto got = ::pread(descriptor_, content.data() + read_total,
                                 content.size() - static_cast<std::size_t>(read_total), read_total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "read sel");
        }
        if (got == 0) {
            break;
        }
        read_total += got;
    }
    std::size_t line_start = 0;
    std::size_t consumed = 0;
    while (line_start < static_cast<std::size_t>(read_total)) {
        const auto newline = std::find(content.begin() + static_cast<std::ptrdiff_t>(line_start),
                                       content.begin() + read_total, '\n');
        if (newline == content.begin() + read_total) {
            // 尾部没有换行：典型的崩溃留下的残缺记录。
            break;
        }
        const std::size_t line_end = static_cast<std::size_t>(newline - content.begin());
        std::string line(content.data() + line_start, line_end - line_start);
        SelRecord record {};
        if (!decode(line, record)) {
            break;
        }
        records_.push_back(std::move(record));
        next_id_ = std::max(next_id_, records_.back().id + 1);
        consumed = line_end + 1;
        line_start = consumed;
    }
    truncated_bytes_ = static_cast<std::uint64_t>(read_total) - consumed;
    if (truncated_bytes_ > 0 && policy_ == Truncate::tail) {
        // 回退到最后一个完整记录，避免残缺行留在文件中间导致后续重放就此停止。
        if (::ftruncate(descriptor_, static_cast<off_t>(consumed)) < 0) {
            throw std::system_error(errno, std::generic_category(), "truncate sel");
        }
    }
    trim();
}

void SelStore::trim() {
    if (records_.size() <= max_records_) {
        return;
    }
    records_.erase(records_.begin(), records_.end() - static_cast<std::ptrdiff_t>(max_records_));
    // 内存与文件都必须只保留最近 max_records 条，否则重启后会把已淘汰的记录又读回来。
    if (policy_ == Truncate::prepare) {
        return;
    }
    // 只对普通文件做重写；管道/设备节点既没有有意义的长度，也不能被 rename 覆盖。
    std::error_code status;
    if (!std::filesystem::is_regular_file(path_, status) || status) {
        return;
    }
    const auto temporary = std::filesystem::path(path_.string() + ".compact");
    {
        const int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (output < 0) {
            throw std::system_error(errno, std::generic_category(), "open sel compaction");
        }
        for (const auto& record : records_) {
            write_all(output, encode(record));
        }
        ::fsync(output);
        ::close(output);
    }
    if (::rename(temporary.c_str(), path_.c_str()) < 0) {
        throw std::system_error(errno, std::generic_category(), "replace sel");
    }
}

void SelStore::drop_oldest(std::size_t count) {
    pending_.erase(0, count);
    dropped_bytes_ += count;
}

bool SelStore::drain(bool sync) {
    if (descriptor_ < 0) {
        return pending_.empty();
    }
    if (pending_.empty()) {
        return true;
    }
    try {
        write_all(descriptor_, pending_);
    } catch (const std::exception&) {
        // 只有写入本身失败才算数据丢失；此时保留缓冲区等待下次重试。
        ++write_failures_;
        return false;
    }
    pending_.clear();
    if (sync && syncable_ && ::fdatasync(descriptor_) < 0) {
        // 同步失败不等于数据丢失（可能只是该文件系统不支持），因此单独计数。
        ++sync_failures_;
    }
    return true;
}

std::uint64_t SelStore::append(const std::string& source, const std::string& state, const std::string& message,
                               std::optional<double> value, bool important) {
    std::lock_guard lock(mutex_);
    const auto id = next_id_++;
    const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    records_.push_back({id, time, source, state, message, value});
    if (records_.size() > max_records_) {
        trim();
    }
    if (policy_ != Truncate::prepare) {
        pending_ += encode(SelRecord{id, time, source, state, message, value});
        if (important || pending_.size() >= batch_bytes_) {
            drain(true);
        }
        if (pending_.size() > pending_cap_) {
            drop_oldest(pending_.size() - pending_cap_);
        }
    }
    return id;
}

std::vector<SelRecord> SelStore::query(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    if (limit >= records_.size()) {
        return records_;
    }
    return {records_.end() - static_cast<std::ptrdiff_t>(limit), records_.end()};
}

std::uint64_t SelStore::next_id() const {
    std::lock_guard lock(mutex_);
    return next_id_;
}

bool SelStore::flush() {
    std::lock_guard lock(mutex_);
    drain(true);
    return pending_.empty();
}

std::size_t SelStore::pending_bytes() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

std::uint64_t SelStore::write_failures() const {
    std::lock_guard lock(mutex_);
    return write_failures_;
}

std::uint64_t SelStore::dropped_bytes() const {
    std::lock_guard lock(mutex_);
    return dropped_bytes_;
}

std::uint64_t SelStore::sync_failures() const {
    std::lock_guard lock(mutex_);
    return sync_failures_;
}

std::uint64_t SelStore::truncated_bytes() const {
    std::lock_guard lock(mutex_);
    return truncated_bytes_;
}

void SelStore::set_pending_cap(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    pending_cap_ = bytes;
}

void SelStore::set_nonblocking(bool enabled) {
    std::lock_guard lock(mutex_);
    if (descriptor_ < 0) {
        throw std::logic_error("sel descriptor is not open");
    }
    const int flags = ::fcntl(descriptor_, F_GETFL);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(), "get sel flags");
    }
    const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(descriptor_, F_SETFL, updated) < 0) {
        throw std::system_error(errno, std::generic_category(), "set sel flags");
    }
}
}
