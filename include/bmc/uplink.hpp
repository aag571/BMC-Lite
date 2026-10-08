#pragma once
// 上行遥测：订阅事件总线、有界队列、非阻塞重连与发送指标。
// 实现见 src/uplink.cpp。
#include "bmc/core.hpp"
#include "bmc/socket_io.hpp"
#include <atomic>

namespace bmc {
// 上行队列与传输计数：dropped 同时包含队列溢出与断线时丢弃的在途帧。
struct UplinkStats {
    std::size_t queued = 0;
    std::uint64_t dropped = 0, sent = 0, bytes = 0, attempts = 0;
    bool connected = false;
};
// 等待队列容量固定；网络线程另外持有最多一条在途消息，每条最多 8 KiB。
// enqueue 只编码并入队，不执行任何网络 I/O；事件总线不能被采集器拖住。
class Uplink {
public:
    using Clock = std::chrono::steady_clock;
    Uplink(std::string address, unsigned port, std::size_t capacity = 256,
           SocketIo& io = system_socket_io());
    ~Uplink();
    // 编码为一行 JSONL 并入队；队列满、已停止或单条超过 8 KiB 时返回 false 并计入 dropped，
    // 绝不阻塞事件总线线程，也不做任何网络 I/O。
    bool enqueue(const BusEvent& event);
    // 启动网络线程；已启动或已停止时抛出 std::logic_error。
    void start();
    // 幂等；停止线程并把队列与在途消息计入 dropped。
    void stop() noexcept;
    UplinkStats stats() const;
    // Prometheus 文本，字段与 UplinkStats 一一对应。
    std::string metrics() const;
    // 单步非阻塞状态机，仅用于未启动线程时的 FakeSocketIo 测试。
    void advance(Clock::time_point now);
private:
    void loop() noexcept;
    void disconnect(Clock::time_point now);
    std::string address_;
    unsigned port_;
    std::size_t capacity_;
    SocketIo& io_;
    mutable std::mutex mutex_;
    std::deque<std::string> queue_;
    UplinkStats stats_;
    std::string pending_;
    std::size_t offset_ = 0;
    int descriptor_ = -1;
    bool connecting_ = false;
    Clock::time_point retry_{}, deadline_{};
    std::chrono::milliseconds backoff_{250};
    std::atomic_bool stopping_{false};
    std::thread thread_;
    std::condition_variable wake_;
};
}
