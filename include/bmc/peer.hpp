#pragma once
// 对等通道：主动心跳、失效判定、代次交换与心跳端点服务。
// 实现见 src/peer.cpp。
#include "bmc/network.hpp"
#include <mutex>

namespace bmc {
// 对等通道只持有代次与时间，不持有 Worker、Action 或设备，结构上禁止心跳触发硬件动作。
class PeerHeartbeat {
public:
    using Clock = std::chrono::steady_clock;
    using Report = std::function<void(const std::string&)>;
    PeerHeartbeat(std::string address, unsigned port, std::string token,
                  std::chrono::milliseconds interval, std::chrono::milliseconds stale,
                  Report report, std::string ca = {}, std::string server_name = {},
                  SocketIo& io = system_socket_io());
    ~PeerHeartbeat();
    // 启动后台线程；与 stop() 配合，同一实例不可重复 start。
    void start();
    // 幂等：停线程、释放 TLS 会话并关闭描述符。
    void stop() noexcept;
    // 更新本端对外的代次号，下一次心跳即携带新值。
    void generation(std::uint64_t value);
    // 服务入站心跳：校验 Bearer 令牌与 /v1/heartbeat 形态后回本端代次，
    // 鉴权失败按来源限流并返回 401/429。可在网络线程外调用，内部加锁。
    HttpResponse handle(const http::Request& request, const std::string& source);
    // 单步驱动可由 FakeSocketIo 和虚拟时间验证，无需真实 socket 或睡眠。
    void advance(Clock::time_point now);
    // Prometheus 文本：失效标志、对端代次、心跳与失败计数。
    std::string metrics() const;
private:
    void observe(std::uint64_t value, Clock::time_point now);
    void disconnect(Clock::time_point now);
    SocketIo& io_;
    std::string address_, token_, server_name_;
    unsigned port_;
    std::chrono::milliseconds interval_, stale_after_;
    Report report_;
    std::unique_ptr<TlsContext> tls_context_;
    std::unique_ptr<TlsSession> tls_session_;
    int descriptor_ = -1;
    bool connecting_ = false;
    std::string output_, input_;
    std::size_t offset_ = 0;
    Clock::time_point next_{}, deadline_{};
    mutable std::mutex mutex_;
    std::uint64_t generation_ = 1, peer_generation_ = 0, received_ = 0, failures_ = 0;
    Clock::time_point last_seen_ = Clock::now();
    bool stale_ = false;
    http::RateLimiter auth_limit_{5, 0.2};
    std::atomic_bool stopping_{false};
    std::thread thread_;
};
}
