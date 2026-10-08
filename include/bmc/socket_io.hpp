#pragma once
#include <cerrno>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace bmc {
// socket 系统调用的最小可注入接口。与 LinuxIo 同样的取向：
// 生产代码用 PosixSocketIo 直通真实 syscall，测试用 FakeSocketIo 脚本化返回值，
// 因此连接状态机、HTTP 解析、限流与认证都能在没有真实端口的情况下单测。
class SocketIo {
public:
    virtual ~SocketIo() = default;
    virtual int socket(int domain, int type, int protocol) = 0;
    virtual int bind(int descriptor, const sockaddr* address, socklen_t length) = 0;
    virtual int listen(int descriptor, int backlog) = 0;
    virtual int accept(int descriptor, sockaddr* address, socklen_t* length) = 0;
    virtual int setsockopt(int descriptor, int level, int name, const void* value, socklen_t length) = 0;
    virtual int getsockopt(int descriptor, int level, int name, void* value, socklen_t* length) = 0;
    virtual ssize_t recv(int descriptor, void* buffer, std::size_t count, int flags) = 0;
    virtual ssize_t send(int descriptor, const void* buffer, std::size_t count, int flags) = 0;
    virtual int shutdown(int descriptor, int how) = 0;
    // 非虚实现：任何实现都必须真正关闭描述符，并在此留下 errno 供诊断。
    int close(int descriptor) noexcept {
        const int result = ::close(descriptor);
        last_error = result < 0 ? errno : 0;
        return result;
    }
    int last_error = 0;
};

class PosixSocketIo final : public SocketIo {
public:
    int socket(int domain, int type, int protocol) override;
    int bind(int descriptor, const sockaddr* address, socklen_t length) override;
    int listen(int descriptor, int backlog) override;
    int accept(int descriptor, sockaddr* address, socklen_t* length) override;
    int setsockopt(int descriptor, int level, int name, const void* value, socklen_t length) override;
    int getsockopt(int descriptor, int level, int name, void* value, socklen_t* length) override;
    ssize_t recv(int descriptor, void* buffer, std::size_t count, int flags) override;
    ssize_t send(int descriptor, const void* buffer, std::size_t count, int flags) override;
    int shutdown(int descriptor, int how) override;
};

// 进程级默认实例：无状态，供不关心注入的调用点与既有测试使用。
SocketIo& system_socket_io();

// 把 sockaddr 格式化成 "ip:port"；解析失败时返回空串。用于审计与限流键。
std::string peer_name(const sockaddr* address, socklen_t length);
}
