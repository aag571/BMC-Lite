#include "bmc/network.hpp"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sstream>

// 只读 / 认证 HTTP 服务端：HttpConnection 与 ReadOnlyServer 都定义在本文件，
// 声明见 include/bmc/network.hpp。每个 ReadOnlyServer 拥有一个 epoll 线程与全部连接状态，
// 不持有传感器、规则或设备，只通过回调取快照、分派请求并上报拒绝。
namespace bmc {
// 连接建立即进入 5 s 读期限；读出完整请求后期限重置为 5 s 写期限，两者不累加。
HttpConnection::HttpConnection(int descriptor, SocketIo& io, Handler handler, Clock::time_point now)
    : descriptor_(descriptor), io_(io), handler_(std::move(handler)), deadline_(now + std::chrono::seconds(5)) {}
// 超时只针对尚未完成的连接；done_ 的连接由事件循环负责关闭与记账。
bool HttpConnection::expired(Clock::time_point now) const { return !done_ && now >= deadline_; }
// 读状态机：非阻塞 recv → 喂增量解析器 → 一次性渲染响应并转入写状态。
// 解析失败分别回 413/501/400；处理器抛异常（通常是审计写失败）回 503 并置 handler_failed_。
void HttpConnection::read(Clock::time_point now) {
    if (done_ || writing()) return;
    std::array<char, 4096> buffer{};
    // 每次就绪最多读 64 KiB，防止单个客户端独占事件循环。
    for (unsigned batch = 0; batch < 16; ++batch) {
        // 一次 recv 的结果：EAGAIN/EWOULDBLOCK 表示已读空，EINTR 立即重试，
        // 其余错误或 count == 0（对方 FIN）都判定连接结束。
        const auto count = io_.recv(descriptor_, buffer.data(), buffer.size(), 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) done_ = true;
            return;
        }
        // 对方 FIN：半条请求不可能再补齐。
        if (count == 0) { done_ = true; return; }
        received_ += static_cast<std::uint64_t>(count);
        // 增量解析：incomplete 时继续 recv，其余结果都意味着本轮必须回应并转入写状态。
        const auto result = parser_.feed(buffer.data(), static_cast<std::size_t>(count));
        if (result == http::ParseResult::incomplete) continue;
        HttpResponse response;
        if (result == http::ParseResult::complete) {
            parsed_ = true;
            try { response = handler_(parser_.request()); }
            catch (const std::exception&) {
                handler_failed_ = true;
                response = {503, "Service Unavailable", "text/plain", "request could not be audited\n"};
            }
        }
        else if (result == http::ParseResult::too_large) response = {413, "Payload Too Large", "text/plain", "request too large\n"};
        else if (result == http::ParseResult::unsupported) response = {501, "Not Implemented", "text/plain", "unsupported request\n"};
        else response = {400, "Bad Request", "text/plain", "bad request\n"};
        // 状态码只用于记账与指标；渲染后的报文进入写状态，由 write() 负责发完。
        status_ = response.status;
        output_ = response.render();
        // 读与写分别采用绝对期限，持续发送小片段不能无限延长期限。
        deadline_ = now + std::chrono::seconds(5);
        return;
    }
}
// 写状态机：从 offset_ 起非阻塞地发完 output_；EAGAIN 只返回，由下一次 EPOLLOUT 续传。
// done_ 仅在整条响应发送完毕后置位。
void HttpConnection::write() {
    if (done_ || !writing()) return;
    const auto count = io_.send(descriptor_, output_.data() + offset_, output_.size() - offset_, MSG_NOSIGNAL);
    if (count < 0) {
        // EAGAIN 只是暂时写不动：保留 offset_，等 epoll 再次报告可写。
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) done_ = true;
        return;
    }
    if (count == 0) { done_ = true; return; }
    offset_ += static_cast<std::size_t>(count);
    sent_ += static_cast<std::uint64_t>(count);
    done_ = offset_ == output_.size();
}
// 只读模式构造：role 为 read、并发上限取头文件默认值 64，不要求 TLS。
ReadOnlyServer::ReadOnlyServer(std::string bind, unsigned port, Snapshot snapshot,
    std::function<std::uint64_t()> samples, SocketIo& io)
    : bind_(std::move(bind)), port_(port), snapshot_(std::move(snapshot)), samples_(std::move(samples)), io_(io) {}
// 析构即停线程：连接状态与回调不得活过服务器对象。
ReadOnlyServer::~ReadOnlyServer() { stop(); }
// 认证模式构造（控制面与心跳）：并发上限收紧到 8，role 默认 control，证书由 start() 校验。
ReadOnlyServer::ReadOnlyServer(std::string bind, unsigned port, Handler handler, Rejection rejection,
    std::string certificate, std::string key, SocketIo& io)
    : bind_(std::move(bind)), port_(port), io_(io), handler_(std::move(handler)),
      rejection_(std::move(rejection)), limit_(8), certificate_(std::move(certificate)), key_(std::move(key)) { role_ = "control"; }
// 连接回收时统一记账：先减活跃数并累加流量，再按 (方法, 状态码) 记一次请求。
// 方法折叠成 GET/POST/other、路径完全不进标签，避免指标基数被客户端左右。
void ReadOnlyServer::account(const HttpConnection& connection) {
    std::lock_guard lock(metrics_mutex_);
    --active_;
    bytes_in_ += connection.received(); bytes_out_ += connection.sent();
    if (connection.status() != 0) {
        auto method = connection.method();
        if (method != "GET" && method != "POST") method = "other";
        ++requests_[{method, connection.status()}];
    }
}
// Prometheus 文本：role 标签区分只读/控制/心跳三类监听，拒绝按 limit/timeout/parse 分因由。
std::string ReadOnlyServer::metrics() const {
    std::lock_guard lock(metrics_mutex_);
    std::ostringstream output;
    output << "bmc_connections_active{role=\"" << role_ << "\"} " << active_ << '\n';
    for (const auto& reason : {std::pair{"limit", rejected_limit_}, {"timeout", rejected_timeout_}, {"parse", rejected_parse_}})
        output << "bmc_connections_rejected_total{role=\"" << role_ << "\",reason=\"" << reason.first << "\"} " << reason.second << '\n';
    for (const auto& [key, count] : requests_)
        output << "bmc_requests_total{role=\"" << role_ << "\",method=\"" << key.first << "\",status=\"" << key.second << "\"} " << count << '\n';
    output << "bmc_request_bytes_total{role=\"" << role_ << "\",direction=\"in\"} " << bytes_in_ << '\n'
           << "bmc_request_bytes_total{role=\"" << role_ << "\",direction=\"out\"} " << bytes_out_ << '\n';
    return output.str();
}
// 启动监听线程：先校验绑定地址与 TLS 组合，再创建 listener/epoll/eventfd 并启动线程。
// 任何一步失败都 stop() 回滚并返回 false，绝不留下半启动状态或已占用的端口。
bool ReadOnlyServer::start(std::string& error) {
    if (port_ == 0) return true;
    if (thread_.joinable()) { error = "already started"; return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (::inet_pton(AF_INET, bind_.c_str(), &address.sin_addr) != 1) {
        error = (handler_ ? role_ : "http") + "-bind requires an IPv4 address"; return false;
    }
    // 认证面（handler_ 非空）要求 TLS 配置自洽；只读面没有这条约束。
    if (handler_) {
        if (certificate_.empty() != key_.empty()) { error = "TLS requires both certificate and key"; return false; }
        if ((ntohl(address.sin_addr.s_addr) >> 24) != 127 && certificate_.empty()) {
            error = "non-loopback " + role_ + " requires TLS"; return false;
        }
        if (!certificate_.empty()) {
            try { tls_ = std::make_unique<TlsContext>(certificate_, key_); }
            catch (const std::exception& failure) { error = failure.what(); return false; }
        }
    }
    // 非阻塞 + CLOEXEC 的监听套接字；SO_REUSEADDR 让重启不受 TIME_WAIT 影响。
    listener_ = io_.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int reuse = 1;
    if (listener_ < 0 || io_.setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        io_.bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        io_.listen(listener_, 64) < 0) { error = std::strerror(errno); stop(); return false; }
    poller_ = ::epoll_create1(EPOLL_CLOEXEC);
    // eventfd 是停止通道：stop() 写一个值就能让 epoll_wait 立即返回，不必等 100 ms 超时。
    wake_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epoll_event event{}; event.events = EPOLLIN; event.data.fd = listener_;
    if (poller_ < 0 || wake_ < 0 || ::epoll_ctl(poller_, EPOLL_CTL_ADD, listener_, &event) < 0) {
        error = std::strerror(errno); stop(); return false;
    }
    event.data.fd = wake_;
    if (::epoll_ctl(poller_, EPOLL_CTL_ADD, wake_, &event) < 0) { error = std::strerror(errno); stop(); return false; }
    // 清掉上一次 stop() 留下的停止标志，使同一实例可以重新启动。
    stopping_ = false;
    // 线程创建失败同样走 stop() 回滚，不会留下已监听的端口。
    try { thread_ = std::thread(&ReadOnlyServer::loop, this); }
    catch (const std::exception& failure) { error = failure.what(); stop(); return false; }
    return true;
}
// 幂等停止：置停止标志并用 eventfd 唤醒事件循环，join 之后才关闭描述符。
// 顺序很关键——fd 必须在线程真正退出后关闭，否则会出现 fd 复用与并发访问。
void ReadOnlyServer::stop() noexcept {
    stopping_ = true;
    if (wake_ >= 0) {
        const std::uint64_t value = 1;
        ssize_t written;
        do { written = ::write(wake_, &value, sizeof(value)); } while (written < 0 && errno == EINTR);
        // EAGAIN 表示已有唤醒事件；其他错误也有 epoll 的 100 ms 周期兜底。
        // 写唤醒失败不影响停止：事件循环最迟在 100 ms 的 epoll 超时后自行退出。
        if (written < 0 && errno != EAGAIN) stopping_.store(true);
    }
    if (thread_.joinable()) thread_.join();
    // 网络线程退出后才关闭描述符，避免 fd 复用和并发访问。
    for (int* descriptor : {&listener_, &wake_, &poller_}) {
        if (*descriptor >= 0) { ::close(*descriptor); *descriptor = -1; }
    }
    tls_.reset();
}
// 事件循环：连接表是线程局部的，连接生命周期完全由本线程管理；
// 其他线程只通过 metrics_mutex_ 读计数。审计失败等异常会置 failed_ 并向上抛，
// 主线程据此以非零退出码结束进程。
void ReadOnlyServer::loop() noexcept {
    struct Client {
        std::unique_ptr<TlsSession> tls;
        std::unique_ptr<HttpConnection> http;
        std::string peer;
        bool rejected = false;
    };
    std::map<int, Client> connections;
    std::array<epoll_event, 64> events{};
    try {
        // 100 ms 超时用于兜底扫描过期连接；wake_ 事件只负责尽快跳出等待。
        while (!stopping_) {
            const int ready = ::epoll_wait(poller_, events.data(), static_cast<int>(events.size()), 100);
            if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("network epoll failed"); }
            const auto now = HttpConnection::Clock::now();
            for (int index = 0; index < ready; ++index) {
                const auto& event = events[static_cast<std::size_t>(index)];
                const int descriptor = event.data.fd;
                if (descriptor == wake_) continue;
                if (descriptor == listener_) {
                    // 每次就绪最多 accept 64 个：连接风暴不能把超时扫描与已有连接的读写饿死。
                    for (unsigned accepted = 0; accepted < 64; ++accepted) {
                        sockaddr_in address{};
                        socklen_t length = sizeof(address);
                        const int client = io_.accept(listener_, reinterpret_cast<sockaddr*>(&address), &length);
                        if (client < 0) { if (errno == EINTR) continue; break; }
                        char text[INET_ADDRSTRLEN]{};
                        const auto* formatted = ::inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
                        const std::string peer = formatted ? text : "unknown";
                        // 达到并发上限：关闭并计数，同时让控制面把这次拒绝写进审计。
                        if (connections.size() >= limit_) {
                            { std::lock_guard lock(metrics_mutex_); ++rejected_limit_; }
                            io_.close(client);
                            if (rejection_) rejection_(peer, "connection-limit");
                            continue;
                        }
                        // 登记成功前描述符仍属于本作用域：提前 continue 或异常都由它关闭。
                        struct PendingClient {
                            int descriptor;
                            SocketIo& io;
                            ~PendingClient() { if (descriptor >= 0) io.close(descriptor); }
                        } guard{client, io_};
                        epoll_event registration{}; registration.events = EPOLLIN; registration.data.fd = client;
                        if (::epoll_ctl(poller_, EPOLL_CTL_ADD, client, &registration) < 0) continue;
                        Client state;
                        state.peer = peer;
                        // TLS 会话本身也是 SocketIo：握手之后 HTTP 读写自动走加密通道。
                        if (tls_) state.tls = std::make_unique<TlsSession>(*tls_, client, io_);
                        auto& transport = state.tls ? static_cast<SocketIo&>(*state.tls) : io_;
                        // 分派回调：认证面把请求与来源 IP 交给 ControlService / PeerHeartbeat，
                        // 只读面在这里直接实现 GET /healthz、/metrics 与 Redfish 资源。
                        state.http = std::make_unique<HttpConnection>(client, transport, [this, peer](const http::Request& request) {
                            if (handler_) return handler_(request, peer);
                            if (request.method != "GET") return HttpResponse{405, "Method Not Allowed", "text/plain", "GET required\n"};
                            if (request.target == "/healthz") return HttpResponse{200, "OK", "application/json",
                                "{\"alive\": true, \"samples\": " + std::to_string(samples_()) + "}\n"};
                            auto response = readonly::handle(request.target, snapshot_());
                            if (request.target.substr(0, request.target.find('?')) == "/metrics" && extra_metrics_)
                                response.body += extra_metrics_();
                            return response;
                        }, now);
                        // 登记成功才交出 fd 所有权，随后 active_ 才递增。
                        connections.emplace(client, std::move(state));
                        guard.descriptor = -1;
                        { std::lock_guard lock(metrics_mutex_); ++active_; }
                    }
                    continue;
                }
                const auto found = connections.find(descriptor);
                if (found == connections.end()) continue;
                auto& state = found->second;
                auto& connection = *state.http;
                // 握手未完成前不解析请求字节：需要的方向由 TlsSession::events() 反馈给 epoll。
                if (state.tls && !state.tls->ready()) state.tls->handshake();
                const bool ready_tls = !state.tls || state.tls->ready();
                if (ready_tls && !connection.writing()) connection.read(now);
                // 转入写状态后不再读：一条连接一次只推进一个方向。
                if (connection.writing()) {
                    connection.write();
                }
                // 认证端点无法完成审计时停止监听；主线程会检测 failed_ 并以非零码退出。
                if (handler_ && connection.handler_failed()) throw std::runtime_error(role_ + " handler failed");
                epoll_event registration{};
                // 按当前阶段重算关注的事件：TLS 未就绪时听 TLS，否则写时关注 EPOLLOUT。
                registration.events = state.tls && state.tls->events() ? state.tls->events() : (connection.writing() ? EPOLLOUT : EPOLLIN);
                registration.data.fd = descriptor;
                ::epoll_ctl(poller_, EPOLL_CTL_MOD, descriptor, &registration);
                // 有响应可写但请求从未解析成功：计入 parse 拒绝，并让控制面写一条审计。
                if (connection.writing() && !connection.parsed() && !state.rejected) {
                    { std::lock_guard lock(metrics_mutex_); ++rejected_parse_; }
                    if (rejection_) rejection_(state.peer, "parse");
                    state.rejected = true;
                }
                // 对端出错/挂断、响应写完或 TLS 失败都视为连接结束；结束前统一记账。
                if ((event.events & (EPOLLERR | EPOLLHUP)) || connection.done() || (state.tls && state.tls->failed())) {
                    if (!connection.parsed() && !state.rejected && rejection_) rejection_(state.peer, "disconnect");
                    account(connection); io_.close(descriptor); connections.erase(found);
                }
            }
            // 每轮再扫一遍超时连接：同样记账，并对认证面补一条 timeout 拒绝。
            for (auto current = connections.begin(); current != connections.end();) {
                if (current->second.http->expired(now)) {
                    { std::lock_guard lock(metrics_mutex_); ++rejected_timeout_; }
                    if (!current->second.http->parsed() && !current->second.rejected && rejection_) rejection_(current->second.peer, "timeout");
                    account(*current->second.http); io_.close(current->first); current = connections.erase(current);
                }
                else ++current;
            }
        }
    // 线程内异常一律置 failed_：认证面无法完成审计时宁可停服，也不能放过未审计的动作。
    } catch (const std::exception& error) {
        failed_ = true;
        std::cerr << "bmc-lite: network thread failed: " << error.what() << '\n';
    } catch (...) {
        failed_ = true;
        std::cerr << "bmc-lite: network thread failed\n";
    }
    // 线程退出前回收所有仍登记的连接，保证 active_ 与字节计数归零。
    for (const auto& entry : connections) { account(*entry.second.http); io_.close(entry.first); }
    // 线程异常后立即撤销监听；stop 在 join 后回收描述符，不再留下无处理线程的端口。
    if (failed_) io_.shutdown(listener_, SHUT_RDWR);
}
}
