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
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// SEL（System Event Log）的持久化实现：文本行记录 + 常驻 O_APPEND 描述符 + 批量落盘，
// 声明见 bmc/core.hpp。内存与文件都只保留最近 max_records 条，启动时还要负责修复尾部残缺记录。
namespace bmc {
namespace {
// 严格解析一个完整数值 token：std::stod 会接受 "1.5abc"，这里要求整段都被消费且结果有限。
double number(const std::string& token) {
    std::size_t consumed = 0;
    const double value = std::stod(token, &consumed);
    if (consumed != token.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid number: " + token);
    }
    return value;
}
// 磁盘格式（每行一条，以 \n 收尾）：
//   <id> <time_ms> "<source>" "<state>" "<message>" <value|null>
// 文本字段经 std::quoted 处理：总是加双引号，并转义其中的 " 与 \，因此字段内可以含空格；
// value 缺失或非有限写 null。行尾换行同时也是"记录完整"的判据（尾部残缺行靠它识别）。
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
// encode 的逆过程。返回 false 表示该行无法解析：重放遇到第一行失败就停止，
// 以免把后续字节错位地当成另一条记录读进来。
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
// 循环写完整个缓冲区：::write 可能短写或返回 EINTR，返回 0 视为短写错误。
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

// 可写性探测：O_CREAT|O_EXCL 只在文件不存在时创建，因此既不会覆盖已有内容，
// 又能在路径不可写时抛出 system_error；EEXIST 表示文件已存在，属于正常结果。
void SelStore::write_probe(const std::filesystem::path& path) {
    const int probe = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (probe < 0 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "open sel");
    }
    if (probe >= 0) {
        ::close(probe);
    }
}

// 打开 SEL 并重放既有记录。sync 为空时用 fdatasync；max_records 同时是内存与文件的上限。
// 重放失败必须关掉描述符再抛出，避免半构造对象留下打开的 fd。
SelStore::SelStore(std::filesystem::path path, std::size_t max_records, Truncate policy, std::function<int(int)> sync)
    : path_(std::move(path)), max_records_(max_records), policy_(policy), sync_(std::move(sync)) {
    if (!sync_) sync_ = [](int descriptor) { return ::fdatasync(descriptor); };
    if (max_records_ == 0) {
        throw std::invalid_argument("zero SEL capacity");
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    std::lock_guard lock(mutex_);
    open();
    try { replay(); }
    catch (...) { ::close(descriptor_); descriptor_ = -1; throw; }
}

// 析构前尽力落盘（drain(true)），失败只计数不抛出；随后关闭常驻描述符。
SelStore::~SelStore() {
    std::lock_guard lock(mutex_);
    drain(true);
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

// 打开常驻描述符，并记录目标是否值得 fsync（只有普通文件才支持 fdatasync）。
void SelStore::open() {
    // 已有文件（例如 /dev/null、已配置好的日志路径）不追加 O_CREAT，
    // 避免在只允许打开既有设备的路径上拿到 EACCES。
    std::error_code status;
    const bool exists = std::filesystem::exists(path_, status);
    const int flags = O_RDWR | O_APPEND | O_CLOEXEC | O_NONBLOCK | (exists ? 0 : O_CREAT);
    descriptor_ = ::open(path_.c_str(), flags, 0644);
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open sel");
    }
    std::error_code type_status;
    syncable_ = std::filesystem::is_regular_file(path_, type_status) && !type_status;
}

// 启动重放：把文件里完整的记录读回内存、续上 id 序列，并按策略处理尾部残缺记录。
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
        // 用 pread 显式指定偏移，读取不会改动 O_APPEND 描述符的写入位置。
        const auto got = ::pread(descriptor_, content.data() + read_total,
                                 content.size() - static_cast<std::size_t>(read_total), read_total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "read sel");
        }
        // 文件比刚查询到的大小时更短（被截断或并发替换），按实际读到的长度重放。
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
        // 解析失败即中止重放：从这一行起的字节都不可信。
        if (!decode(line, record)) {
            break;
        }
        records_.push_back(std::move(record));
        // id 从已有最大值 +1 续接，保证重启后不会重用出现过的编号。
        next_id_ = std::max(next_id_, records_.back().id + 1);
        consumed = line_end + 1;
        line_start = consumed;
    }
    // 残缺字节 = 最后一个完整记录之后的所有内容（可能是半行，也可能是多行半截数据）。
    truncated_bytes_ = static_cast<std::uint64_t>(read_total) - consumed;
    // 只有 tail 策略会回退文件；refuse 保留残缺行（可能让后续重放停在这里，但绝不丢数据），
    // prepare 既不截断也不落盘，只用于测试与只读检查。
    if (truncated_bytes_ > 0 && policy_ == Truncate::tail) {
        // 回退到最后一个完整记录，避免残缺行留在文件中间导致后续重放就此停止。
        if (::ftruncate(descriptor_, static_cast<off_t>(consumed)) < 0) {
            throw std::system_error(errno, std::generic_category(), "truncate sel");
        }
    }
    // 重放完成后按容量上限裁剪，并把裁剪结果同步回文件。
    trim();
}

// 把内存记录裁剪到 max_records_ 条，并在策略允许时重写文件，让磁盘与内存保持一致。
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
    // 临时文件建在同一目录：与目标同处一个文件系统，rename 才是原子替换。
    auto temporary = path_.string() + ".compact.XXXXXX";
    const int output = ::mkstemp(temporary.data());
    try {
        if (output < 0) {
            throw std::system_error(errno, std::generic_category(), "open sel compaction");
        }
        // 让临时文件继承目标的属性：CLOEXEC、追加语义与 0644 权限。
        if (::fcntl(output, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(output, F_SETFL, O_APPEND | O_NONBLOCK) < 0 || ::fchmod(output, 0644) < 0)
            throw std::system_error(errno, std::generic_category(), "configure sel compaction");
        for (const auto& record : records_) {
            write_all(output, encode(record));
        }
        // 先把新内容真正落盘，再改名，否则崩溃后可能出现"名字指向空文件"。
        if (::fsync(output) < 0) { ++sync_failures_; throw std::system_error(errno, std::generic_category(), "sync sel compaction"); }
        if (::rename(temporary.c_str(), path_.c_str()) < 0)
            throw std::system_error(errno, std::generic_category(), "replace sel");
    } catch (...) {
        // 任何失败都要关掉临时 fd 并删除临时文件，不留残骸。
        if (output >= 0) ::close(output);
        ::unlink(temporary.c_str());
        throw;
    }
    // rename 不会更新已打开的 fd；直接接管新文件，避免后续写入已被删除的旧 inode。
    ::close(descriptor_); descriptor_ = output;
    // 快照包含所有内存记录，包括原缓冲区和本次追加，不能再次追加这些行。
    pending_.clear();
    const auto parent = path_.parent_path().empty() ? std::filesystem::path(".") : path_.parent_path();
    Fd directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    // 目录项本身也要 fsync，否则改名可能只停留在页缓存里，掉电后仍指向旧 inode。
    if (directory.get() < 0 || ::fsync(directory.get()) < 0) ++sync_failures_;
}

// 丢弃最旧的未落盘字节并计数；调用方保证 count 不超过缓冲区长度。
void SelStore::drop_oldest(std::size_t count) {
    pending_.erase(0, count);
    dropped_bytes_ += count;
}

// 把缓冲区写出去；返回 false 表示仍有残留。sync 只在目标可同步时执行。
bool SelStore::drain(bool sync) {
    if (descriptor_ < 0) {
        return pending_.empty();
    }
    if (pending_.empty()) {
        return true;
    }
    while (!pending_.empty()) {
        const auto written = ::write(descriptor_, pending_.data(), pending_.size());
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ++write_failures_; return false; }
        // 短写后只保留尚未写出的后缀，重试不能把已写出的半条记录再写一次。
        pending_.erase(0, static_cast<std::size_t>(written));
    }
    if (sync && syncable_ && sync_(descriptor_) < 0) {
        // 同步失败不等于数据丢失（可能只是该文件系统不支持），因此单独计数。
        ++sync_failures_;
    }
    return true;
}

// 追加一条记录：分配单调递增的 id，写内存与缓冲区，必要时裁剪/落盘，最后限制缓冲区上限。
// important=true 表示该记录是故障证据，必须立刻 write+fdatasync，而不是等批量阈值。
std::uint64_t SelStore::append(const std::string& source, const std::string& state, const std::string& message,
                               std::optional<double> value, bool important) {
    std::lock_guard lock(mutex_);
    const auto id = next_id_++;
    const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    records_.push_back({id, time, source, state, message, value});
    // prepare 策略只维护内存视图，不产生待落盘内容。
    if (policy_ != Truncate::prepare) {
        pending_ += encode(SelRecord{id, time, source, state, message, value});
    }
    // 裁剪会重写文件、可能失败；不能让它中断 append，只记一次写入失败。
    if (records_.size() > max_records_) {
        try { trim(); }
        catch (const std::exception&) { ++write_failures_; }
    }
    // important=true 立即落盘；否则等缓冲区达到 batch_bytes_（默认 4096）。
    if (policy_ != Truncate::prepare) {
        if (important || pending_.size() >= batch_bytes_) {
            drain(true);
        }
        // 上限默认 1 MiB，超出后丢弃最旧的未落盘字节，保证内存占用有界。
        if (pending_.size() > pending_cap_) {
            drop_oldest(pending_.size() - pending_cap_);
        }
    }
    return id;
}

// 返回最近 limit 条记录的快照（拷贝）；limit 不小于总条数时返回全部。
std::vector<SelRecord> SelStore::query(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    if (limit >= records_.size()) {
        return records_;
    }
    return {records_.end() - static_cast<std::ptrdiff_t>(limit), records_.end()};
}

// 下一个待分配的 id；调用方用它判断记录是否连续。
std::uint64_t SelStore::next_id() const {
    std::lock_guard lock(mutex_);
    return next_id_;
}

// 主动落盘并同步；缓冲区仍非空时返回 false。
bool SelStore::flush() {
    std::lock_guard lock(mutex_);
    drain(true);
    return pending_.empty();
}

// 尚未成功落盘的字节数。
std::size_t SelStore::pending_bytes() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

// 写入失败次数；Monitor 以它的增长触发一次降级上报。
std::uint64_t SelStore::write_failures() const {
    std::lock_guard lock(mutex_);
    return write_failures_;
}

// 因缓冲区超限而被丢弃的字节数。
std::uint64_t SelStore::dropped_bytes() const {
    std::lock_guard lock(mutex_);
    return dropped_bytes_;
}

// 同步失败次数，与写入失败分开计数：写入进了页缓存但 fdatasync 失败，并不等于数据丢失。
std::uint64_t SelStore::sync_failures() const {
    std::lock_guard lock(mutex_);
    return sync_failures_;
}

// 启动重放时发现的尾部残缺字节数；只有 tail 策略会真正修复文件。
std::uint64_t SelStore::truncated_bytes() const {
    std::lock_guard lock(mutex_);
    return truncated_bytes_;
}

// 调整未落盘缓冲区的上限；测试它压到很小以覆盖"丢弃最旧内容"的路径。
void SelStore::set_pending_cap(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    pending_cap_ = bytes;
}

// 切换 O_NONBLOCK：管道之类的目标写满时会阻塞，开启后写失败立即返回，便于快速降级与测试。
// 描述符未打开时无法改标志，抛 std::logic_error。
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
