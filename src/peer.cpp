#include "bmc/peer.hpp"
#include "bmc/control.hpp"
#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <stdexcept>

// 对等心跳：PeerHeartbeat 的连接/握手/请求/响应状态机定义在本文件，声明见 include/bmc/peer.hpp。
// 出方向每 interval_ 心跳一次，入方向应答对端同样的请求；结构上不持有 Worker、Action
// 或设备，因此心跳不可能触发硬件动作。
namespace bmc {
namespace {
// 代次必须是完整消费的十进制且非 0；空串、负号、尾随字符与 0 都判非法。
bool parse_generation(const std::string& text, std::uint64_t& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0;
}
}
// 构造即校验：地址为 IPv4、端口合法、令牌 32..512 字符、interval < stale 且有上报回调；
// CA 与 server_name 必须成对给出，非回环对端还必须有 CA（即要求校验过的 TLS）。
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
// 析构必须先停线程：否则网络线程会继续使用已析构的成员。
PeerHeartbeat::~PeerHeartbeat() { stop(); }
// 心跳线程以 10 ms 周期驱动 advance()；单步异常折算为一次断开，线程本身不退出。
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
// 幂等停止：先置标志再 join，之后才释放 TLS 会话与描述符，避免线程访问已释放对象。
void PeerHeartbeat::stop() noexcept {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    tls_session_.reset();
    if (descriptor_ >= 0) { io_.close(descriptor_); descriptor_ = -1; }
}
// 由控制面在配置热重载后写入；下一次心跳会带上这个新代次。
void PeerHeartbeat::generation(std::uint64_t value) { std::lock_guard lock(mutex_); generation_ = value; }
// 收到一次合法心跳：刷新 last_seen_、记录对端代次并清除 stale 标志。
// recovered 只在真正从 stale 变回 fresh 的那一次为真，因此恢复只上报一次。
void PeerHeartbeat::observe(std::uint64_t value, Clock::time_point now) {
    bool recovered = false;
    {
        std::lock_guard lock(mutex_);
        last_seen_ = now; peer_generation_ = value; ++received_;
        recovered = stale_;
        stale_ = false;
    }
    if (recovered) report_("peer recovered generation=" + std::to_string(value));
}
// 入方向处理对端心跳，顺序固定：先令牌（失败按来源 IP 限流），再校验 method/target/
// 空 body 与 X-BMC-Generation，最后才更新观测状态并回本机代次。
HttpResponse PeerHeartbeat::handle(const http::Request& request, const std::string& source) {
    if (!token_equal("Bearer " + token_, request.header("Authorization"))) {
        std::lock_guard lock(mutex_);
        // 限流键是来源 IP（网络层传入的 source 不含端口），重连换端口无法绕过。
        const bool allowed = auth_limit_.allow(source);
        return {allowed ? 401 : 429, allowed ? "Unauthorized" : "Too Many Requests", "text/plain", "authentication rejected\n"};
    }
    std::uint64_t value = 0;
    if (request.method != "GET" || request.target != "/v1/heartbeat" || !request.body.empty() ||
        !parse_generation(request.header("X-BMC-Generation"), value))
        return {400, "Bad Request", "text/plain", "invalid heartbeat\n"};
    // 校验全部通过才观测：非 0 代次已由 parse_generation 保证。
    observe(value, Clock::now());
    std::lock_guard lock(mutex_);
    return {200, "OK", "text/plain", std::to_string(generation_) + "\n"};
}
// 统一的失败出口：释放 TLS 会话、关闭描述符、清空收发缓冲，并按 interval_ 安排下一次心跳。
void PeerHeartbeat::disconnect(Clock::time_point now) {
    tls_session_.reset();
    if (descriptor_ >= 0) io_.close(descriptor_);
    descriptor_ = -1; input_.clear(); output_.clear(); offset_ = 0; connecting_ = false;
    next_ = now + interval_;
}
// 单步状态机：判 stale → 连接 → TLS 握手 → 发请求 → 收响应并结束连接。
// 整个交换共享一个绝对期限 min(5 s, stale_after_)，零碎进展不能延长它。
void PeerHeartbeat::advance(Clock::time_point now) {
    // stale 是锁存状态：只在跨越阈值的那一次上报，避免每轮心跳重复刷事件。
    std::optional<std::uint64_t> stale_generation;
    {
        std::lock_guard lock(mutex_);
        if (!stale_ && now - last_seen_ >= stale_after_) {
            stale_ = true;
            stale_generation = peer_generation_;
        }
    }
    if (stale_generation) report_("peer stale generation=" + std::to_string(*stale_generation));
    // 本步的所有失败都走这里：失败计数加一，然后统一断开。
    auto failed = [&] { { std::lock_guard lock(mutex_); ++failures_; } disconnect(now); };
    if (descriptor_ < 0) {
        // 按 interval_ 节流：距上次心跳不足一个间隔就不重连。
        if (now < next_) return;
        descriptor_ = io_.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (descriptor_ < 0) { failed(); return; }
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::inet_pton(AF_INET, address_.c_str(), &address.sin_addr);
        // 非阻塞 connect：EINPROGRESS 表示仍在握手，其余错误立即算一次失败。
        const int result = io_.connect(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result < 0 && errno != EINPROGRESS) { failed(); return; }
        connecting_ = result < 0;
        // 一次完整连接、TLS、请求、响应共享绝对期限，不因零碎数据延长。
        deadline_ = now + std::min(stale_after_, std::chrono::milliseconds(5000));
        { std::lock_guard lock(mutex_);
          // 请求头一次性构造成字符串，之后只按 offset_ 续发；Connection: close 表示一次一连接。
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
        // EAGAIN 只返回，等下一次可写；其余错误算一次失败。
        if (sent < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) failed(); return; }
        if (sent == 0) { failed(); return; }
        offset_ += static_cast<std::size_t>(sent);
        // 请求没发完就不读响应：先写后读，避免半双工下把回显当成响应。
        if (offset_ < output_.size()) return;
    }
    std::array<char, 1024> buffer{};
    // 每次最多读 1 KiB，累计超过 8 KiB 视为异常响应并断开。
    const auto count = transport.recv(descriptor_, buffer.data(), buffer.size(), 0);
    if (count < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) failed(); return; }
    if (count > 0) {
        input_.append(buffer.data(), static_cast<std::size_t>(count));
        if (input_.size() > 8192) { failed(); return; }
    }
    // 按 Content-Length 接收短响应；TLS 服务端关闭前不必等待 close_notify。
    const auto separator = input_.find("\r\n\r\n");
    // 响应体是以换行结尾的短文本；未见到换行且连接仍开着时继续读。
    if (count > 0 && (separator == std::string::npos || input_.size() == separator + 4 || input_.back() != '\n')) return;
    std::uint64_t value = 0;
    // 只接受 200 状态行 + 单个非 0 代次；任何一项不满足都算一次失败。
    if (input_.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 || separator == std::string::npos || input_.back() != '\n' ||
        !parse_generation(input_.substr(separator + 4, input_.size() - separator - 5), value)) { failed(); return; }
    const auto body_size = input_.size() - separator - 4;
    // 头部里的 Content-Length 必须与实际体长一致，防止被截断或粘包的响应蒙混过关。
    if (input_.substr(0, separator).find("\r\nContent-Length: " + std::to_string(body_size) + "\r\n") == std::string::npos) {
        failed(); return;
    }
    // 一次成功交换后主动断开：每个心跳都是独立连接，不留复用状态。
    observe(value, now); disconnect(now);
}
// Prometheus 文本：stale 标志、本机与对端代次、成功心跳数与失败次数。
std::string PeerHeartbeat::metrics() const {
    std::lock_guard lock(mutex_);
    return "bmc_peer_stale " + std::to_string(stale_) + "\nbmc_peer_generation " + std::to_string(peer_generation_) +
        "\nbmc_peer_heartbeats_total " + std::to_string(received_) + "\nbmc_peer_failures_total " + std::to_string(failures_) + "\n";
}
}
