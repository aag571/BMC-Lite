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

// JSON 行日志的滚动写入实现（EventLogger 的文件后端），声明见 bmc/core.hpp。
// 常驻描述符 + 批量合并：默认不 fsync，仅在缓冲区达到 batch_bytes 或需要轮转时才真正写盘。
namespace bmc {
namespace {
// 时间戳用 Unix 毫秒（system_clock），与 SEL 的 time_ms 及外部解析脚本保持一致。
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(milliseconds);
}
}
// 打开日志文件并校验滚动策略：limit/keep 为 0 或 keep>100 直接拒绝（分片数量必须有限），
// batch 为 0 则批量合并无从触发；父目录缺失时先创建，open 失败以 system_error 抛出。
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
// 析构时尽力把缓冲区落盘；析构期间不能抛异常，所以失败只累加计数。
Logger::~Logger() {
    std::lock_guard lock(mutex_);
    // 尽力把缓冲区落盘；失败也不能在这里抛异常（析构期间）。
    drain(false);
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}
// 以追加方式打开：O_APPEND 让并发写入不会互相覆盖，O_CLOEXEC 防止描述符泄漏给子进程，
// O_NONBLOCK 让管道之类的目标在写满时立即返回而不是卡住采样线程。
void Logger::open() {
    descriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0644);
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open log");
    }
}
// 把待落盘缓冲区写出去；返回 false 表示缓冲区仍有残留（写入失败或只写出一部分）。
// 失败只累加 write_failures_，由 Monitor 的降级上报暴露出来，不在采样热路径抛异常。
bool Logger::drain(bool sync) {
    // 描述符不可用（未打开或已关闭）：此时只能报告缓冲区是否已经空了。
    if (descriptor_ < 0) {
        return pending_.empty();
    }
    if (pending_.empty()) {
        return true;
    }
    while (!pending_.empty()) {
        const auto written = ::write(descriptor_, pending_.data(), pending_.size());
        if (written < 0 && errno == EINTR) continue;
        // 部分写入时只保留尚未写出的后缀，重试不会把已写出的内容再写一遍。
        if (written <= 0) { ++write_failures_; return false; }
        pending_.erase(0, static_cast<std::size_t>(written));
    }
    if (sync && ::fdatasync(descriptor_) < 0) {
        // 同步失败不表示写入失败，但仍记录失败次数以便观测。
        ++write_failures_;
    }
    return true;
}
// 未落盘缓冲区超过上限（默认 1 MiB）时丢弃最旧的字节，保留最新日志；
// 同时计入丢弃字节数与一次失败，让"日志已经不完整"这件事也能被降级上报发现。
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
// 按需滚动分片。返回 false 表示本次未能安全轮转（缓冲区没能先落盘），
// 调用方必须保持文件与缓冲区原样并立刻重试写入，否则这批行会随改名一起丢失。
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
// 追加一条已经格式化好的 JSON 行。rotate 先于入缓冲执行，让新记录的长度参与本次阈值判断；
// 之后按需落盘，最后裁剪超出上限的未落盘内容。
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
    // 四种需要立刻落盘的情形：轮转失败（必须重试）、显式 sync 模式、目标大小不可查询
    // （管道/设备节点，见 rotate 的说明）、缓冲区达到了批量阈值。
    if (!rotated || policy_.sync || status || pending_.size() >= policy_.batch_bytes) {
        drain(policy_.sync);
    }
    drop_oldest_excess();
}
// 主动把缓冲区写盘并（按策略）同步；缓冲区仍非空时返回 false。
bool Logger::flush() {
    std::lock_guard lock(mutex_);
    drain(policy_.sync);
    return pending_.empty();
}
// 未落盘字节数；与 drop_oldest_excess 的计数一起构成降级上报的输入。
std::size_t Logger::pending_bytes() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}
// 累计写入失败次数（含丢弃未落盘内容）；供 EventLogger 接口的降级上报读取。
std::uint64_t Logger::write_failures() const {
    std::lock_guard lock(mutex_);
    return write_failures_;
}
// 因超过 pending_cap 被丢弃的字节数，用于把"日志缺失量"量化出来。
std::uint64_t Logger::dropped_bytes() const {
    std::lock_guard lock(mutex_);
    return dropped_bytes_;
}
// 把状态迁移事件序列化成一行 JSON。value 缺失或非有限时写 null；
// 精度 17 保证 double 能原样往返（与 SEL、JSON 写出器的约定一致）。
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
// 动作/服务审计行：字段比事件行少，只有 time_ms / sensor / action 三个键。
void Logger::action(const std::string& id, const std::string& result) {
    append("{\"time_ms\":" + timestamp() + ",\"sensor\":\"" + escape(id) + "\",\"action\":\"" + escape(result) + "\"}");
}
}
