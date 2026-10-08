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

namespace bmc {
HttpConnection::HttpConnection(int descriptor, SocketIo& io, Handler handler, Clock::time_point now)
    : descriptor_(descriptor), io_(io), handler_(std::move(handler)), deadline_(now + std::chrono::seconds(5)) {}
bool HttpConnection::expired(Clock::time_point now) const { return !done_ && now >= deadline_; }
void HttpConnection::read(Clock::time_point now) {
    if (done_ || writing()) return;
    std::array<char, 4096> buffer{};
    // 每次就绪最多读 64 KiB，防止单个客户端独占事件循环。
    for (unsigned batch = 0; batch < 16; ++batch) {
        const auto count = io_.recv(descriptor_, buffer.data(), buffer.size(), 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) done_ = true;
            return;
        }
        if (count == 0) { done_ = true; return; }
        received_ += static_cast<std::uint64_t>(count);
        const auto result = parser_.feed(buffer.data(), static_cast<std::size_t>(count));
        if (result == http::ParseResult::incomplete) continue;
        HttpResponse response;
        if (result == http::ParseResult::complete) {
            parsed_ = true;
            try { response = handler_(parser_.request()); }
            catch (const std::exception&) { response = {503, "Service Unavailable", "text/plain", "request could not be audited\n"}; }
        }
        else if (result == http::ParseResult::too_large) response = {413, "Payload Too Large", "text/plain", "request too large\n"};
        else if (result == http::ParseResult::unsupported) response = {501, "Not Implemented", "text/plain", "unsupported request\n"};
        else response = {400, "Bad Request", "text/plain", "bad request\n"};
        status_ = response.status;
        output_ = response.render();
        // 读与写分别采用绝对期限，持续发送小片段不能无限延长期限。
        deadline_ = now + std::chrono::seconds(5);
        return;
    }
}
void HttpConnection::write() {
    if (done_ || !writing()) return;
    const auto count = io_.send(descriptor_, output_.data() + offset_, output_.size() - offset_, MSG_NOSIGNAL);
    if (count < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) done_ = true;
        return;
    }
    if (count == 0) { done_ = true; return; }
    offset_ += static_cast<std::size_t>(count);
    sent_ += static_cast<std::uint64_t>(count);
    done_ = offset_ == output_.size();
}
ReadOnlyServer::ReadOnlyServer(std::string bind, unsigned port, Snapshot snapshot,
    std::function<std::uint64_t()> samples, SocketIo& io)
    : bind_(std::move(bind)), port_(port), snapshot_(std::move(snapshot)), samples_(std::move(samples)), io_(io) {}
ReadOnlyServer::~ReadOnlyServer() { stop(); }
ReadOnlyServer::ReadOnlyServer(std::string bind, unsigned port, Handler handler, Rejection rejection,
    std::string certificate, std::string key, SocketIo& io)
    : bind_(std::move(bind)), port_(port), io_(io), handler_(std::move(handler)),
      rejection_(std::move(rejection)), limit_(8), certificate_(std::move(certificate)), key_(std::move(key)) { role_ = "control"; }
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
bool ReadOnlyServer::start(std::string& error) {
    if (port_ == 0) return true;
    if (thread_.joinable()) { error = "already started"; return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (::inet_pton(AF_INET, bind_.c_str(), &address.sin_addr) != 1) { error = "http-bind requires an IPv4 address"; return false; }
    if (handler_) {
        if (certificate_.empty() != key_.empty()) { error = "TLS requires both certificate and key"; return false; }
        if ((ntohl(address.sin_addr.s_addr) >> 24) != 127 && certificate_.empty()) {
            error = "non-loopback control requires TLS"; return false;
        }
        if (!certificate_.empty()) {
            try { tls_ = std::make_unique<TlsContext>(certificate_, key_); }
            catch (const std::exception& failure) { error = failure.what(); return false; }
        }
    }
    listener_ = io_.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int reuse = 1;
    if (listener_ < 0 || io_.setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        io_.bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        io_.listen(listener_, 64) < 0) { error = std::strerror(errno); stop(); return false; }
    poller_ = ::epoll_create1(EPOLL_CLOEXEC);
    wake_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epoll_event event{}; event.events = EPOLLIN; event.data.fd = listener_;
    if (poller_ < 0 || wake_ < 0 || ::epoll_ctl(poller_, EPOLL_CTL_ADD, listener_, &event) < 0) {
        error = std::strerror(errno); stop(); return false;
    }
    event.data.fd = wake_;
    if (::epoll_ctl(poller_, EPOLL_CTL_ADD, wake_, &event) < 0) { error = std::strerror(errno); stop(); return false; }
    stopping_ = false;
    try { thread_ = std::thread(&ReadOnlyServer::loop, this); }
    catch (const std::exception& failure) { error = failure.what(); stop(); return false; }
    return true;
}
void ReadOnlyServer::stop() noexcept {
    stopping_ = true;
    if (wake_ >= 0) {
        const std::uint64_t value = 1;
        ssize_t written;
        do { written = ::write(wake_, &value, sizeof(value)); } while (written < 0 && errno == EINTR);
        // EAGAIN 表示已有唤醒事件；其他错误也有 epoll 的 100 ms 周期兜底。
        if (written < 0 && errno != EAGAIN) stopping_.store(true);
    }
    if (thread_.joinable()) thread_.join();
    // 网络线程退出后才关闭描述符，避免 fd 复用和并发访问。
    for (int* descriptor : {&listener_, &wake_, &poller_}) {
        if (*descriptor >= 0) { ::close(*descriptor); *descriptor = -1; }
    }
    tls_.reset();
}
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
        while (!stopping_) {
            const int ready = ::epoll_wait(poller_, events.data(), static_cast<int>(events.size()), 100);
            if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("network epoll failed"); }
            const auto now = HttpConnection::Clock::now();
            for (int index = 0; index < ready; ++index) {
                const auto& event = events[static_cast<std::size_t>(index)];
                const int descriptor = event.data.fd;
                if (descriptor == wake_) continue;
                if (descriptor == listener_) {
                    for (unsigned accepted = 0; accepted < 64; ++accepted) {
                        sockaddr_in address{};
                        socklen_t length = sizeof(address);
                        const int client = io_.accept(listener_, reinterpret_cast<sockaddr*>(&address), &length);
                        if (client < 0) { if (errno == EINTR) continue; break; }
                        char text[INET_ADDRSTRLEN]{};
                        const auto* formatted = ::inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
                        const std::string peer = formatted ? text : "unknown";
                        if (connections.size() >= limit_) {
                            { std::lock_guard lock(metrics_mutex_); ++rejected_limit_; }
                            io_.close(client);
                            if (rejection_) rejection_(peer, "connection-limit");
                            continue;
                        }
                        struct PendingClient {
                            int descriptor;
                            SocketIo& io;
                            ~PendingClient() { if (descriptor >= 0) io.close(descriptor); }
                        } guard{client, io_};
                        epoll_event registration{}; registration.events = EPOLLIN; registration.data.fd = client;
                        if (::epoll_ctl(poller_, EPOLL_CTL_ADD, client, &registration) < 0) continue;
                        Client state;
                        state.peer = peer;
                        if (tls_) state.tls = std::make_unique<TlsSession>(*tls_, client, io_);
                        auto& transport = state.tls ? static_cast<SocketIo&>(*state.tls) : io_;
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
                if (state.tls && !state.tls->ready()) state.tls->handshake();
                const bool ready_tls = !state.tls || state.tls->ready();
                if (ready_tls && !connection.writing()) connection.read(now);
                if (connection.writing()) {
                    connection.write();
                }
                epoll_event registration{};
                registration.events = state.tls && state.tls->events() ? state.tls->events() : (connection.writing() ? EPOLLOUT : EPOLLIN);
                registration.data.fd = descriptor;
                ::epoll_ctl(poller_, EPOLL_CTL_MOD, descriptor, &registration);
                if (connection.writing() && !connection.parsed() && !state.rejected) {
                    { std::lock_guard lock(metrics_mutex_); ++rejected_parse_; }
                    if (rejection_) rejection_(state.peer, "parse");
                    state.rejected = true;
                }
                if ((event.events & (EPOLLERR | EPOLLHUP)) || connection.done() || (state.tls && state.tls->failed())) {
                    if (!connection.parsed() && !state.rejected && rejection_) rejection_(state.peer, "disconnect");
                    account(connection); io_.close(descriptor); connections.erase(found);
                }
            }
            for (auto current = connections.begin(); current != connections.end();) {
                if (current->second.http->expired(now)) {
                    { std::lock_guard lock(metrics_mutex_); ++rejected_timeout_; }
                    if (!current->second.http->parsed() && !current->second.rejected && rejection_) rejection_(current->second.peer, "timeout");
                    account(*current->second.http); io_.close(current->first); current = connections.erase(current);
                }
                else ++current;
            }
        }
    } catch (const std::exception& error) {
        failed_ = true;
        std::cerr << "bmc-lite: network thread failed: " << error.what() << '\n';
    } catch (...) {
        failed_ = true;
        std::cerr << "bmc-lite: network thread failed\n";
    }
    for (const auto& entry : connections) { account(*entry.second.http); io_.close(entry.first); }
    // 线程异常后立即撤销监听；stop 在 join 后回收描述符，不再留下无处理线程的端口。
    if (failed_) io_.shutdown(listener_, SHUT_RDWR);
}
}
