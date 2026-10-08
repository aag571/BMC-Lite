#include "bmc/socket_io.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// socket 系统调用直通层与地址格式化：PosixSocketIo 与 peer_name 定义在本文件，
// 声明见 include/bmc/socket_io.hpp。这里不含任何连接状态机——那些在 src/network.cpp、
// src/uplink.cpp 与 src/peer.cpp 中，因此可以用 FakeSocketIo 单独验证。
namespace bmc {
// 直通 ::socket；失败时把 errno 存进原子的 last_error 供诊断，调用方仍以返回值判断。
int PosixSocketIo::socket(int domain, int type, int protocol) { const int r = ::socket(domain, type, protocol); last_error = r < 0 ? errno : 0; return r; }
// 直通 ::bind；与下面的 listen/setsockopt 一样不更新 last_error，失败信息由调用方读 errno。
int PosixSocketIo::bind(int descriptor, const sockaddr* address, socklen_t length) { return ::bind(descriptor, address, length); }
// 直通 ::listen；backlog 由调用方决定（只读服务端用 64）。
int PosixSocketIo::listen(int descriptor, int backlog) { return ::listen(descriptor, backlog); }
// 用 accept4 一次拿到已连接的套接字，并直接置为非阻塞与 CLOEXEC，避免事后 fcntl 的竞态。
int PosixSocketIo::accept(int descriptor, sockaddr* address, socklen_t* length) {
    const int result = ::accept4(descriptor, address, length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    last_error = result < 0 ? errno : 0;
    return result;
}
// 直通 ::setsockopt；SO_REUSEADDR 与 SO_SNDBUF 都走这里，失败不更新 last_error。
int PosixSocketIo::setsockopt(int descriptor, int level, int name, const void* value, socklen_t length) {
    return ::setsockopt(descriptor, level, name, value, length);
}
// 直通 ::getsockopt；非阻塞连接的完成判定就靠读取 SO_ERROR，因此这里记录 last_error。
int PosixSocketIo::getsockopt(int descriptor, int level, int name, void* value, socklen_t* length) {
    const int r = ::getsockopt(descriptor, level, name, value, length); last_error = r < 0 ? errno : 0; return r;
}
// 直通 ::recv；不重试也不解释错误，EAGAIN/EWOULDBLOCK 原样交给上层状态机。
ssize_t PosixSocketIo::recv(int descriptor, void* buffer, std::size_t count, int flags) {
    return ::recv(descriptor, buffer, count, flags);
}
// 直通 ::send；MSG_NOSIGNAL 由调用方传入，进程另外还忽略了 SIGPIPE。
ssize_t PosixSocketIo::send(int descriptor, const void* buffer, std::size_t count, int flags) {
    return ::send(descriptor, buffer, count, flags);
}
// 直通 ::shutdown；TLS 会话把该调用转发给底层套接字。
int PosixSocketIo::shutdown(int descriptor, int how) { return ::shutdown(descriptor, how); }

// 进程级唯一实例：PosixSocketIo 无状态，多个网络线程共享它是安全的（last_error 是原子）。
SocketIo& system_socket_io() {
    static PosixSocketIo instance;
    return instance;
}

// 把 sockaddr 格式化成 "ip:port"（IPv6 加方括号）；地址为空、协议族未知、长度不足或
// inet_ntop 失败都返回空串。返回值带源端口，因此它是审计标识而不是稳定的限流键：
// 按 IP 聚合的限流键由 src/network.cpp 的 accept 循环自行构造（只取 inet_ntop 结果）。
std::string peer_name(const sockaddr* address, socklen_t length) {
    if (address == nullptr) {
        return {};
    }
    if (address->sa_family == AF_INET && length >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) == nullptr) {
            return {};
        }
        return std::string(text) + ":" + std::to_string(ntohs(ipv4->sin_port));
    }
    if (address->sa_family == AF_INET6 && length >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        char text[INET6_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text)) == nullptr) {
            return {};
        }
        return std::string("[") + text + "]:" + std::to_string(ntohs(ipv6->sin6_port));
    }
    return {};
}
}
