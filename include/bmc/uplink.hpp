#pragma once
#include "bmc/core.hpp"
#include "bmc/socket_io.hpp"
#include <atomic>

namespace bmc {
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
    bool enqueue(const BusEvent& event);
    void start();
    void stop() noexcept;
    UplinkStats stats() const;
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
