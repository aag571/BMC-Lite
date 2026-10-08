#pragma once
#include "bmc/http.hpp"
#include "bmc/service.hpp"
#include "bmc/tls.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <map>
#include <mutex>

namespace bmc {
// 状态机不拥有描述符，便于 FakeSocketIo 测试；服务器负责关闭连接。
class HttpConnection {
public:
    using Clock = std::chrono::steady_clock;
    using Handler = std::function<HttpResponse(const http::Request&)>;
    HttpConnection(int descriptor, SocketIo& io, Handler handler, Clock::time_point now);
    void read(Clock::time_point now);
    void write();
    bool expired(Clock::time_point now) const;
    bool writing() const { return !output_.empty(); }
    bool done() const { return done_; }
    int status() const { return status_; }
    bool parsed() const { return parsed_; }
    bool handler_failed() const { return handler_failed_; }
    std::string method() const { return parser_.request().method; }
    std::uint64_t received() const { return received_; }
    std::uint64_t sent() const { return sent_; }
private:
    int descriptor_;
    SocketIo& io_;
    Handler handler_;
    http::Parser parser_;
    Clock::time_point deadline_;
    std::string output_;
    std::size_t offset_ = 0;
    bool done_ = false;
    bool parsed_ = false;
    bool handler_failed_ = false;
    int status_ = 0;
    std::uint64_t received_ = 0, sent_ = 0;
};

// 独立 epoll 线程，只调用快照回调；不持有传感器、规则引擎或硬件对象。
class ReadOnlyServer {
public:
    using Handler = std::function<HttpResponse(const http::Request&, const std::string&)>;
    using Rejection = std::function<void(const std::string&, const std::string&)>;
    using Snapshot = std::function<std::vector<SelEntry>()>;
    ReadOnlyServer(std::string bind, unsigned port, Snapshot snapshot,
                   std::function<std::uint64_t()> samples, SocketIo& io = system_socket_io());
    ~ReadOnlyServer();
    ReadOnlyServer(std::string bind, unsigned port, Handler handler, Rejection rejection,
                   std::string certificate, std::string key, SocketIo& io = system_socket_io());
    bool start(std::string& error);
    // 启动前设置指标快照回调，运行中不改变回调对象。
    void extra_metrics(std::function<std::string()> callback) { extra_metrics_ = std::move(callback); }
    void stop() noexcept;
    std::string metrics() const;
    void role(std::string value) { role_ = std::move(value); }
    bool failed() const { return failed_.load(); }
private:
    void loop() noexcept;
    std::string bind_;
    unsigned port_;
    Snapshot snapshot_;
    std::function<std::uint64_t()> samples_;
    SocketIo& io_;
    int listener_ = -1, poller_ = -1, wake_ = -1;
    std::atomic_bool stopping_ = false;
    std::thread thread_;
    Handler handler_;
    Rejection rejection_;
    std::size_t limit_ = 64;
    std::string certificate_, key_;
    std::unique_ptr<TlsContext> tls_;
    std::function<std::string()> extra_metrics_;
    // 指标仅保存固定方法及有限状态码，不使用客户端路径作为标签，避免基数膨胀。
    mutable std::mutex metrics_mutex_;
    std::string role_ = "read";
    std::uint64_t active_ = 0, rejected_limit_ = 0, rejected_timeout_ = 0, rejected_parse_ = 0;
    std::uint64_t bytes_in_ = 0, bytes_out_ = 0;
    std::map<std::pair<std::string, int>, std::uint64_t> requests_;
    std::atomic_bool failed_{false};
    void account(const HttpConnection& connection);
};
}
