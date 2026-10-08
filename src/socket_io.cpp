#include "bmc/socket_io.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bmc {
int PosixSocketIo::socket(int domain, int type, int protocol) { return ::socket(domain, type, protocol); }
int PosixSocketIo::bind(int descriptor, const sockaddr* address, socklen_t length) { return ::bind(descriptor, address, length); }
int PosixSocketIo::listen(int descriptor, int backlog) { return ::listen(descriptor, backlog); }
int PosixSocketIo::accept(int descriptor, sockaddr* address, socklen_t* length) { return ::accept(descriptor, address, length); }
int PosixSocketIo::setsockopt(int descriptor, int level, int name, const void* value, socklen_t length) {
    return ::setsockopt(descriptor, level, name, value, length);
}
int PosixSocketIo::getsockopt(int descriptor, int level, int name, void* value, socklen_t* length) {
    return ::getsockopt(descriptor, level, name, value, length);
}
ssize_t PosixSocketIo::recv(int descriptor, void* buffer, std::size_t count, int flags) {
    return ::recv(descriptor, buffer, count, flags);
}
ssize_t PosixSocketIo::send(int descriptor, const void* buffer, std::size_t count, int flags) {
    return ::send(descriptor, buffer, count, flags);
}
int PosixSocketIo::shutdown(int descriptor, int how) { return ::shutdown(descriptor, how); }

SocketIo& system_socket_io() {
    static PosixSocketIo instance;
    return instance;
}

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
