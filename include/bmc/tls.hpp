#pragma once
// TLS 服务端/客户端上下文与每连接的 SocketIo 装饰器。
// 实现见 src/tls.cpp。
#include "bmc/socket_io.hpp"
#include <memory>
namespace bmc {
// OpenSSL 上下文的所有者。服务器形态持有证书与私钥，客户端形态（单一 CA 参数）
// 校验对端证书。证书、私钥或 CA 无效，以及构建时未启用 TLS，构造都会抛出 std::runtime_error。
class TlsContext {
public:
    TlsContext(const std::string& certificate, const std::string& key);
    explicit TlsContext(const std::string& ca_file);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    // 底层 SSL_CTX*，供需要 OpenSSL 细节的调用点使用；不是 SSL 对象。
    void* native() const { return context_; }
private:
    void* context_ = nullptr;
};
// 每连接独立 SSL 状态；WANT_READ/WANT_WRITE 交给 epoll，绝不阻塞网络线程。
class TlsSession final : public SocketIo {
public:
    TlsSession(TlsContext& context, int descriptor, SocketIo& underlying, const std::string& server_name = {});
    ~TlsSession();
    // 非阻塞推进握手；需要更多事件时返回 false（events() 给出关注方向），失败置 failed()。
    bool handshake();
    bool ready() const { return ready_; }
    bool failed() const { return failed_; }
    // 握手尚未完成时值得等待的 epoll 事件（EPOLLIN/EPOLLOUT）；握手完成后固定为 EPOLLIN。
    std::uint32_t events() const { return events_; }
    int socket(int domain, int type, int protocol) override { return underlying_.socket(domain, type, protocol); }
    int bind(int fd, const sockaddr* address, socklen_t length) override { return underlying_.bind(fd, address, length); }
    int listen(int fd, int backlog) override { return underlying_.listen(fd, backlog); }
    int accept(int fd, sockaddr* address, socklen_t* length) override { return underlying_.accept(fd, address, length); }
    int setsockopt(int fd, int level, int name, const void* value, socklen_t length) override { return underlying_.setsockopt(fd, level, name, value, length); }
    int getsockopt(int fd, int level, int name, void* value, socklen_t* length) override { return underlying_.getsockopt(fd, level, name, value, length); }
    int shutdown(int fd, int how) override { return underlying_.shutdown(fd, how); }
    // 数据面走 TLS；需要更多事件时把 SSL 的 WANT_READ/WANT_WRITE 折算为 errno=EAGAIN 加 -1，
    // 方向由 events() 反映。除 recv/send 外的系统调用一律直通 underlying_（描述符本就属于它）。
    ssize_t recv(int fd, void* buffer, std::size_t count, int flags) override;
    ssize_t send(int fd, const void* buffer, std::size_t count, int flags) override;
private:
    ssize_t result(int code, std::size_t count);
    void* session_ = nullptr;
    SocketIo& underlying_;
    bool ready_ = false, failed_ = false;
    std::uint32_t events_ = 0;
};
}
