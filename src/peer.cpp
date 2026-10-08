#include "bmc/peer.hpp"
#include "bmc/control.hpp"
#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <stdexcept>

namespace bmc {
namespace {
bool parse_generation(const std::string& text, std::uint64_t& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0;
}
}
PeerHeartbeat::PeerHeartbeat(std::string address, unsigned port, std::string token,
    std::chrono::milliseconds interval, std::chrono::milliseconds stale, Report report,
    std::string ca, std::string server_name, SocketIo& io)
    : io_(io), address_(std::move(address)), token_(std::move(token)), server_name_(std::move(server_name)),
      port_(port), interval_(interval), stale_after_(stale), report_(std::move(report)) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, address_.c_str(), &parsed) != 1 || port == 0 || port > 65535 ||
        token_.size() < 32 || token_.size() > 512 || interval.count() < 10 || stale <= interval || !report_)
        throw std::invalid_argument("invalid peer heartbeat configuration");
    if (ca.empty() != server_name_.empty()) throw std::invalid_argument("peer TLS requires CA and server name together");
    if ((ntohl(parsed.s_addr) >> 24) != 127 && ca.empty()) throw std::invalid_argument("remote peer requires verified TLS");
    if (!ca.empty()) tls_context_ = std::make_unique<TlsContext>(ca);
}
PeerHeartbeat::~PeerHeartbeat() { stop(); }
void PeerHeartbeat::start() {
    if (thread_.joinable()) throw std::logic_error("peer already started");
    stopping_ = false;
    thread_ = std::thread([this] {
        while (!stopping_) {
            try { advance(Clock::now()); }
            catch (const std::exception&) { disconnect(Clock::now()); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}
void PeerHeartbeat::stop() noexcept {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    tls_session_.reset();
    if (descriptor_ >= 0) { io_.close(descriptor_); descriptor_ = -1; }
}
void PeerHeartbeat::generation(std::uint64_t value) { std::lock_guard lock(mutex_); generation_ = value; }
void PeerHeartbeat::observe(std::uint64_t value, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    last_seen_ = now; peer_generation_ = value; ++received_;
    if (stale_) { report_("peer recovered generation=" + std::to_string(value)); stale_ = false; }
}
HttpResponse PeerHeartbeat::handle(const http::Request& request, const std::string& source) {
    if (!token_equal("Bearer " + token_, request.header("Authorization"))) {
        std::lock_guard lock(mutex_);
        const bool allowed = auth_limit_.allow(source);
        return {allowed ? 401 : 429, allowed ? "Unauthorized" : "Too Many Requests", "text/plain", "authentication rejected\n"};
    }
    std::uint64_t value = 0;
    if (request.method != "GET" || request.target != "/v1/heartbeat" || !request.body.empty() ||
        !parse_generation(request.header("X-BMC-Generation"), value))
        return {400, "Bad Request", "text/plain", "invalid heartbeat\n"};
    observe(value, Clock::now());
    std::lock_guard lock(mutex_);
    return {200, "OK", "text/plain", std::to_string(generation_) + "\n"};
}
void PeerHeartbeat::disconnect(Clock::time_point now) {
    tls_session_.reset();
    if (descriptor_ >= 0) io_.close(descriptor_);
    descriptor_ = -1; input_.clear(); output_.clear(); offset_ = 0; connecting_ = false;
    next_ = now + interval_;
}
void PeerHeartbeat::advance(Clock::time_point now) {
    {
        std::lock_guard lock(mutex_);
        if (!stale_ && now - last_seen_ >= stale_after_) {
            stale_ = true; report_("peer stale generation=" + std::to_string(peer_generation_));
        }
    }
    auto failed = [&] { { std::lock_guard lock(mutex_); ++failures_; } disconnect(now); };
    if (descriptor_ < 0) {
        if (now < next_) return;
        descriptor_ = io_.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (descriptor_ < 0) { failed(); return; }
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::inet_pton(AF_INET, address_.c_str(), &address.sin_addr);
        const int result = io_.connect(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result < 0 && errno != EINPROGRESS) { failed(); return; }
        connecting_ = result < 0;
        // 一次完整连接、TLS、请求、响应共享绝对期限，不因零碎数据延长。
        deadline_ = now + std::min(stale_after_, std::chrono::milliseconds(5000));
        { std::lock_guard lock(mutex_);
          output_ = "GET /v1/heartbeat HTTP/1.1\r\nHost: " + address_ + "\r\nAuthorization: Bearer " + token_ +
                    "\r\nX-BMC-Generation: " + std::to_string(generation_) + "\r\nConnection: close\r\n\r\n"; }
    }
    if (now >= deadline_) { failed(); return; }
    if (connecting_) {
        const int ready = io_.writable(descriptor_);
        if (ready == 0 || (ready < 0 && errno == EINTR)) return;
        int error = 0; socklen_t length = sizeof(error);
        if (ready < 0 || io_.getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) { failed(); return; }
        connecting_ = false;
    }
    if (tls_context_ && !tls_session_) tls_session_ = std::make_unique<TlsSession>(*tls_context_, descriptor_, io_, server_name_);
    if (tls_session_ && !tls_session_->ready()) {
        tls_session_->handshake();
        if (tls_session_->failed()) { failed(); return; }
        if (!tls_session_->ready()) return;
    }
    auto& transport = tls_session_ ? static_cast<SocketIo&>(*tls_session_) : io_;
    if (offset_ < output_.size()) {
        const auto sent = transport.send(descriptor_, output_.data() + offset_, output_.size() - offset_, MSG_NOSIGNAL);
        if (sent < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) failed(); return; }
        if (sent == 0) { failed(); return; }
        offset_ += static_cast<std::size_t>(sent);
        if (offset_ < output_.size()) return;
    }
    std::array<char, 1024> buffer{};
    const auto count = transport.recv(descriptor_, buffer.data(), buffer.size(), 0);
    if (count < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) failed(); return; }
    if (count > 0) {
        input_.append(buffer.data(), static_cast<std::size_t>(count));
        if (input_.size() > 8192) { failed(); return; }
    }
    // 按 Content-Length 接收短响应；TLS 服务端关闭前不必等待 close_notify。
    const auto separator = input_.find("\r\n\r\n");
    if (count > 0 && (separator == std::string::npos || input_.size() == separator + 4 || input_.back() != '\n')) return;
    std::uint64_t value = 0;
    if (input_.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 || separator == std::string::npos || input_.back() != '\n' ||
        !parse_generation(input_.substr(separator + 4, input_.size() - separator - 5), value)) { failed(); return; }
    const auto body_size = input_.size() - separator - 4;
    if (input_.substr(0, separator).find("\r\nContent-Length: " + std::to_string(body_size) + "\r\n") == std::string::npos) {
        failed(); return;
    }
    observe(value, now); disconnect(now);
}
std::string PeerHeartbeat::metrics() const {
    std::lock_guard lock(mutex_);
    return "bmc_peer_stale " + std::to_string(stale_) + "\nbmc_peer_generation " + std::to_string(peer_generation_) +
        "\nbmc_peer_heartbeats_total " + std::to_string(received_) + "\nbmc_peer_failures_total " + std::to_string(failures_) + "\n";
}
}
