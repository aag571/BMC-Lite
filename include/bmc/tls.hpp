#pragma once
#include "bmc/socket_io.hpp"
#include <memory>
namespace bmc {
class TlsContext {
public:
    TlsContext(const std::string& certificate, const std::string& key);
    explicit TlsContext(const std::string& ca_file);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    void* native() const { return context_; }
private:
    void* context_ = nullptr;
};
// 每连接独立 SSL 状态；WANT_READ/WANT_WRITE 交给 epoll，绝不阻塞网络线程。
class TlsSession final : public SocketIo {
public:
    TlsSession(TlsContext& context, int descriptor, SocketIo& underlying, const std::string& server_name = {});
    ~TlsSession();
    bool handshake();
    bool ready() const { return ready_; }
    bool failed() const { return failed_; }
    std::uint32_t events() const { return events_; }
    int socket(int domain, int type, int protocol) override { return underlying_.socket(domain, type, protocol); }
    int bind(int fd, const sockaddr* address, socklen_t length) override { return underlying_.bind(fd, address, length); }
    int listen(int fd, int backlog) override { return underlying_.listen(fd, backlog); }
    int accept(int fd, sockaddr* address, socklen_t* length) override { return underlying_.accept(fd, address, length); }
    int setsockopt(int fd, int level, int name, const void* value, socklen_t length) override { return underlying_.setsockopt(fd, level, name, value, length); }
    int getsockopt(int fd, int level, int name, void* value, socklen_t* length) override { return underlying_.getsockopt(fd, level, name, value, length); }
    int shutdown(int fd, int how) override { return underlying_.shutdown(fd, how); }
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
