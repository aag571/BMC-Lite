#include "bmc/tls.hpp"
#include <cerrno>
#include <stdexcept>
#include <sys/epoll.h>
#include <arpa/inet.h>
#ifdef BMC_TLS
#include <openssl/ssl.h>
#endif
namespace bmc {
TlsContext::TlsContext(const std::string& certificate, const std::string& key) {
#ifdef BMC_TLS
    auto* context = SSL_CTX_new(TLS_server_method());
    if (!context) throw std::runtime_error("TLS context creation failed");
    if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(context, certificate.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(context, key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context) != 1) {
        SSL_CTX_free(context);
        throw std::runtime_error("invalid TLS certificate or private key");
    }
    SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
    context_ = context;
#else
    static_cast<void>(certificate); static_cast<void>(key);
    throw std::runtime_error("TLS support was disabled at build time");
#endif
}
TlsContext::~TlsContext() {
#ifdef BMC_TLS
    SSL_CTX_free(static_cast<SSL_CTX*>(context_));
#endif
}
TlsContext::TlsContext(const std::string& ca_file) {
#ifdef BMC_TLS
    auto* context = SSL_CTX_new(TLS_client_method());
    if (!context) throw std::runtime_error("TLS client context creation failed");
    if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
        SSL_CTX_load_verify_locations(context, ca_file.c_str(), nullptr) != 1) {
        SSL_CTX_free(context);
        throw std::runtime_error("invalid peer CA file");
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
    context_ = context;
#else
    static_cast<void>(ca_file);
    throw std::runtime_error("TLS support was disabled at build time");
#endif
}
TlsSession::TlsSession(TlsContext& context, int descriptor, SocketIo& underlying, const std::string& server_name) : underlying_(underlying) {
    events_ = EPOLLIN;
#ifdef BMC_TLS
    auto* session = SSL_new(static_cast<SSL_CTX*>(context.native()));
    if (!session) throw std::runtime_error("TLS session creation failed");
    if (SSL_set_fd(session, descriptor) != 1) { SSL_free(session); throw std::runtime_error("TLS socket assignment failed"); }
    if (server_name.empty()) SSL_set_accept_state(session);
    else {
        // IP 用证书 IP SAN 校验，DNS 名同时设置 SNI 和主机名校验；不提供跳过验证选项。
        in_addr address{};
        const bool numeric = ::inet_pton(AF_INET, server_name.c_str(), &address) == 1;
        const int verified = numeric ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(session), server_name.c_str())
                                     : SSL_set1_host(session, server_name.c_str());
        if (verified != 1 || (!numeric && SSL_set_tlsext_host_name(session, server_name.c_str()) != 1)) {
            SSL_free(session); throw std::runtime_error("invalid peer TLS server name");
        }
        SSL_set_connect_state(session);
    }
    session_ = session;
#else
    static_cast<void>(context); static_cast<void>(descriptor); static_cast<void>(server_name);
    throw std::runtime_error("TLS disabled");
#endif
}
TlsSession::~TlsSession() {
#ifdef BMC_TLS
    SSL_free(static_cast<SSL*>(session_));
#endif
}
ssize_t TlsSession::result(int code, std::size_t count) {
#ifdef BMC_TLS
    if (code == 1) { events_ = 0; return static_cast<ssize_t>(count); }
    const int error = SSL_get_error(static_cast<SSL*>(session_), code);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        events_ = error == SSL_ERROR_WANT_READ ? EPOLLIN : EPOLLOUT;
        errno = EAGAIN; return -1;
    }
    failed_ = true;
    if (error == SSL_ERROR_ZERO_RETURN) return 0;
#else
    static_cast<void>(code); static_cast<void>(count);
#endif
    errno = EIO; return -1;
}
bool TlsSession::handshake() {
#ifdef BMC_TLS
    const int code = SSL_do_handshake(static_cast<SSL*>(session_));
    if (code == 1) { ready_ = true; events_ = EPOLLIN; return true; }
    result(code, 0);
#endif
    return false;
}
ssize_t TlsSession::recv(int, void* buffer, std::size_t count, int) {
#ifdef BMC_TLS
    std::size_t received = 0;
    const int code = SSL_read_ex(static_cast<SSL*>(session_), buffer, count, &received);
    return result(code, received);
#else
    static_cast<void>(buffer); static_cast<void>(count); errno = EIO; return -1;
#endif
}
ssize_t TlsSession::send(int, const void* buffer, std::size_t count, int) {
#ifdef BMC_TLS
    std::size_t sent = 0;
    const int code = SSL_write_ex(static_cast<SSL*>(session_), buffer, count, &sent);
    return result(code, sent);
#else
    static_cast<void>(buffer); static_cast<void>(count); errno = EIO; return -1;
#endif
}
}
