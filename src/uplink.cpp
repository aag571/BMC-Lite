#include "bmc/uplink.hpp"
#include "bmc/json.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <iostream>
#include <netinet/in.h>
#include <sstream>

// 上行通道：Uplink 的单向 JSONL 状态机定义在本文件，声明见 include/bmc/uplink.hpp。
// enqueue 只在调用线程里编码入队，网络 I/O 全部由自己的线程在 advance() 中推进，
// 因此采集器再慢也不会阻塞事件总线或采样循环。
namespace bmc {
namespace {
// BusEventType 到线上字符串的稳定映射；未知类型写 "unknown"，便于下游识别协议漂移。
const char* event_type(BusEventType type) {
    switch (type) {
        case BusEventType::sensor_state: return "sensor_state";
        case BusEventType::configuration: return "configuration";
        case BusEventType::service: return "service";
        case BusEventType::recovery: return "recovery";
    }
    return "unknown";
}
}
// 构造即校验 IPv4、端口与队列容量：容量上限 4096，保证等待队列占用有界。
Uplink::Uplink(std::string address, unsigned port, std::size_t capacity, SocketIo& io)
    : address_(std::move(address)), port_(port), capacity_(capacity), io_(io) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, address_.c_str(), &parsed) != 1 || port == 0 || port > 65535 ||
        capacity == 0 || capacity > 4096) throw std::invalid_argument("invalid uplink IPv4, port or capacity");
}
// 析构停止线程并丢弃未发送内容，网络线程不得活过对象。
Uplink::~Uplink() { stop(); }
// 只做"限长 → 编码 → 入队"，绝不碰 socket：调用方是事件总线的分发线程。
// 队列满时丢弃最旧的一条并计数，最新状态优先上行。
bool Uplink::enqueue(const BusEvent& event) {
    if (stopping_) return false;
    // 先限制原始字段，再编码，避免异常长事件造成编码缓冲区瞬时增长。
    if (event.source.size() + event.message.size() > 4096) {
        std::lock_guard lock(mutex_); ++stats_.dropped; return false;
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto value = JsonWriter::Value::object();
    value.set("type", JsonWriter::Value::of(std::string(event_type(event.type))))
         .set("source", JsonWriter::Value::of(event.source))
         .set("message", JsonWriter::Value::of(event.message))
         .set("sequence", JsonWriter::Value::of(static_cast<std::int64_t>(event.sequence)))
         .set("time_ms", JsonWriter::Value::of(milliseconds))
         // 非有限值写成 null：JSON 没有 NaN/Infinity，否则下游会解析出非法文档。
         .set("value", event.value && std::isfinite(*event.value) ? JsonWriter::Value::of(*event.value) : JsonWriter::Value::null());
    auto line = JsonWriter::dump(value) + "\n";
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    // 单行上限 8 KiB：超过说明事件本身异常，丢弃而不是截断（截断会破坏 JSONL）。
    if (line.size() > 8192) { ++stats_.dropped; return false; }
    // 队列满时丢最旧的一条：上行是状态观测通道，保留最新比保留历史更有价值。
    if (queue_.size() == capacity_) { queue_.pop_front(); ++stats_.dropped; }
    queue_.push_back(std::move(line));
    wake_.notify_one();
    return true;
}
// 启动网络线程；重复启动或已停止的实例直接抛 std::logic_error，不静默忽略。
void Uplink::start() {
    if (thread_.joinable() || stopping_) throw std::logic_error("uplink already started or stopped");
    thread_ = std::thread(&Uplink::loop, this);
}
// 幂等停止：置标志唤醒线程，join 之后关闭描述符，并把队列与在途帧计入 dropped。
void Uplink::stop() noexcept {
    stopping_ = true; wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (descriptor_ >= 0) { io_.close(descriptor_); descriptor_ = -1; }
    std::lock_guard lock(mutex_);
    stats_.dropped += queue_.size() + (pending_.empty() ? 0u : 1u);
    queue_.clear(); pending_.clear(); stats_.connected = false;
}
// 快照式统计：顺带把当前队列深度填进结果，调用方不必再取一次锁。
UplinkStats Uplink::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_; result.queued = queue_.size(); return result;
}
// Prometheus 文本：队列深度、丢弃/发送/字节/连接尝试与当前连接状态。
std::string Uplink::metrics() const {
    const auto state = stats();
    std::ostringstream output;
    output << "bmc_uplink_queued " << state.queued << '\n'
           << "bmc_uplink_dropped_total " << state.dropped << '\n'
           << "bmc_uplink_sent_total " << state.sent << '\n'
           << "bmc_uplink_bytes_total " << state.bytes << '\n'
           << "bmc_uplink_connect_attempts_total " << state.attempts << '\n'
           << "bmc_uplink_connected " << (state.connected ? 1 : 0) << '\n';
    return output.str();
}
// 统一的失败出口：关闭描述符、弃掉在途帧，并按指数退避安排下次连接（250 ms 起，封顶 30 s）。
// 退避只在这里推进；只有一整条消息真正发完才会复位。
void Uplink::disconnect(Clock::time_point now) {
    if (descriptor_ >= 0) { io_.close(descriptor_); descriptor_ = -1; }
    connecting_ = false;
    std::lock_guard lock(mutex_);
    stats_.connected = false;
    // 未发送完的帧丢弃：新 TCP 流不能从 JSON 行的中间继续。
    if (!pending_.empty()) { ++stats_.dropped; pending_.clear(); offset_ = 0; }
    retry_ = now + backoff_;
    backoff_ = std::min(backoff_ * 2, std::chrono::milliseconds(30000));
}
// 单步非阻塞状态机：连接 → 等可写 → 空闲时探测对端 FIN → 取队首 → 最多 16 个发送片段。
// 任何错误都收敛到 disconnect()，这里不阻塞、不重试、不睡眠。
void Uplink::advance(Clock::time_point now) {
    if (stopping_) return;
    if (descriptor_ < 0) {
        // 未到退避时间就直接返回；本函数由 20 ms 轮询驱动，不在这里等待。
        if (now < retry_) return;
        { std::lock_guard lock(mutex_); ++stats_.attempts; }
        // 每次尝试都计入 attempts；socket 或发送缓冲设置失败都按断开处理，等退避重来。
        descriptor_ = io_.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (descriptor_ < 0) { disconnect(now); return; }
        int buffer = 16384;
        if (io_.setsockopt(descriptor_, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) < 0) { disconnect(now); return; }
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::inet_pton(AF_INET, address_.c_str(), &address.sin_addr);
        // 非阻塞 connect：EINPROGRESS 表示握手仍在进行，EINTR 也按进行中处理。
        const int result = io_.connect(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result < 0 && errno != EINPROGRESS && errno != EINTR) { disconnect(now); return; }
        connecting_ = result < 0;
        // 连接与首次发送共享 5 s 绝对期限：零碎的可写事件不能延长它。
        deadline_ = now + std::chrono::seconds(5);
        if (!connecting_) { std::lock_guard lock(mutex_); stats_.connected = true; }
    }
    if (connecting_) {
        if (now >= deadline_) { disconnect(now); return; }
        // 用 poll 零超时 + SO_ERROR 判定非阻塞连接的结果，绝不阻塞网络线程。
        const int ready = io_.writable(descriptor_);
        if (ready == 0 || (ready < 0 && errno == EINTR)) return;
        int error = 0; socklen_t size = sizeof(error);
        if (ready < 0 || io_.getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0) {
            disconnect(now); return;
        }
        connecting_ = false;
        std::lock_guard lock(mutex_); stats_.connected = true;
    }
    if (pending_.empty()) {
        bool empty;
        { std::lock_guard lock(mutex_); empty = queue_.empty(); }
        // 队列为空时也要看一眼对端：读到 FIN/RST 说明采集器已断开，应主动重连。
        if (empty) {
            char unexpected;
            // MSG_PEEK 不消费字节：0 表示 FIN，正数表示协议外数据，两者都要重连。
            const auto count = io_.recv(descriptor_, &unexpected, 1, MSG_PEEK | MSG_DONTWAIT);
            // 单向 JSONL 协议不接收响应；即使没有新事件，也检测采集器的 FIN/RST。
            if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) disconnect(now);
            return;
        }
    }
    // 每轮最多发送 16 个片段，网络工作量有界；部分写入不会占住队列锁。
    for (unsigned batch = 0; batch < 16; ++batch) {
        if (pending_.empty()) {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) return;
            // 队列锁只覆盖出队这一下：send 在锁外执行，部分写入不会占住队列。
            pending_ = std::move(queue_.front()); queue_.pop_front();
            offset_ = 0; deadline_ = now + std::chrono::seconds(5);
        }
        if (now >= deadline_) { disconnect(now); return; }
        // 期限到即断开：慢速对端不能把一帧拖成永久占用。
        const auto count = io_.send(descriptor_, pending_.data() + offset_, pending_.size() - offset_, MSG_NOSIGNAL);
        if (count < 0) {
            // 暂时写不动就返回，由下一轮轮询续传；其余错误断开重连。
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            disconnect(now); return;
        }
        if (count == 0) { disconnect(now); return; }
        offset_ += static_cast<std::size_t>(count);
        { std::lock_guard lock(mutex_); stats_.bytes += static_cast<std::uint64_t>(count); }
        if (offset_ == pending_.size()) {
            // 一整条消息发完才算连接可用：退避复位，下次失败重新从 250 ms 起。
            pending_.clear(); offset_ = 0; backoff_ = std::chrono::milliseconds(250);
            std::lock_guard lock(mutex_); ++stats_.sent;
        }
    }
}
// 网络线程：以 20 ms 为周期推进状态机；条件变量只用于停止，
// 高事件速率下被唤醒也不会把周期变成忙等。
void Uplink::loop() noexcept {
    try {
        while (!stopping_) {
            advance(Clock::now());
            std::unique_lock lock(mutex_);
            // 条件仅响应停止；高事件速率不能绕过 20 ms 网络轮询节奏造成忙等。
            wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stopping_.load(); });
        }
    // 线程内异常只记录并让线程退出，不再向进程传播。
    } catch (const std::exception& error) {
        std::cerr << "bmc-lite: uplink failed: " << error.what() << '\n';
    }
}
}
