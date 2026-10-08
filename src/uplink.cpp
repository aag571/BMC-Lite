#include "bmc/uplink.hpp"
#include "bmc/json.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <iostream>
#include <netinet/in.h>
#include <sstream>

namespace bmc {
namespace {
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
Uplink::Uplink(std::string address, unsigned port, std::size_t capacity, SocketIo& io)
    : address_(std::move(address)), port_(port), capacity_(capacity), io_(io) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, address_.c_str(), &parsed) != 1 || port == 0 || port > 65535 ||
        capacity == 0 || capacity > 4096) throw std::invalid_argument("invalid uplink IPv4, port or capacity");
}
Uplink::~Uplink() { stop(); }
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
         .set("value", event.value && std::isfinite(*event.value) ? JsonWriter::Value::of(*event.value) : JsonWriter::Value::null());
    auto line = JsonWriter::dump(value) + "\n";
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    if (line.size() > 8192) { ++stats_.dropped; return false; }
    if (queue_.size() == capacity_) { queue_.pop_front(); ++stats_.dropped; }
    queue_.push_back(std::move(line));
    wake_.notify_one();
    return true;
}
void Uplink::start() {
    if (thread_.joinable() || stopping_) throw std::logic_error("uplink already started or stopped");
    thread_ = std::thread(&Uplink::loop, this);
}
void Uplink::stop() noexcept {
    stopping_ = true; wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (descriptor_ >= 0) { io_.close(descriptor_); descriptor_ = -1; }
    std::lock_guard lock(mutex_);
    stats_.dropped += queue_.size() + (pending_.empty() ? 0u : 1u);
    queue_.clear(); pending_.clear(); stats_.connected = false;
}
UplinkStats Uplink::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_; result.queued = queue_.size(); return result;
}
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
void Uplink::advance(Clock::time_point now) {
    if (stopping_) return;
    if (descriptor_ < 0) {
        if (now < retry_) return;
        { std::lock_guard lock(mutex_); ++stats_.attempts; }
        descriptor_ = io_.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (descriptor_ < 0) { disconnect(now); return; }
        int buffer = 16384;
        if (io_.setsockopt(descriptor_, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) < 0) { disconnect(now); return; }
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::inet_pton(AF_INET, address_.c_str(), &address.sin_addr);
        const int result = io_.connect(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result < 0 && errno != EINPROGRESS && errno != EINTR) { disconnect(now); return; }
        connecting_ = result < 0;
        deadline_ = now + std::chrono::seconds(5);
        if (!connecting_) { std::lock_guard lock(mutex_); stats_.connected = true; }
    }
    if (connecting_) {
        if (now >= deadline_) { disconnect(now); return; }
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
        if (empty) {
            char unexpected;
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
            pending_ = std::move(queue_.front()); queue_.pop_front();
            offset_ = 0; deadline_ = now + std::chrono::seconds(5);
        }
        if (now >= deadline_) { disconnect(now); return; }
        const auto count = io_.send(descriptor_, pending_.data() + offset_, pending_.size() - offset_, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            disconnect(now); return;
        }
        if (count == 0) { disconnect(now); return; }
        offset_ += static_cast<std::size_t>(count);
        { std::lock_guard lock(mutex_); stats_.bytes += static_cast<std::uint64_t>(count); }
        if (offset_ == pending_.size()) {
            pending_.clear(); offset_ = 0; backoff_ = std::chrono::milliseconds(250);
            std::lock_guard lock(mutex_); ++stats_.sent;
        }
    }
}
void Uplink::loop() noexcept {
    try {
        while (!stopping_) {
            advance(Clock::now());
            std::unique_lock lock(mutex_);
            // 条件仅响应停止；高事件速率不能绕过 20 ms 网络轮询节奏造成忙等。
            wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stopping_.load(); });
        }
    } catch (const std::exception& error) {
        std::cerr << "bmc-lite: uplink failed: " << error.what() << '\n';
    }
}
}
