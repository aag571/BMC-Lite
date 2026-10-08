#include "bmc/tls.hpp"
#include <cerrno>
#include <stdexcept>
#include <sys/epoll.h>
#include <arpa/inet.h>
#ifdef BMC_TLS
#include <openssl/ssl.h>
#endif
// tls.cpp —— OpenSSL 服务端/客户端上下文与每连接会话（声明见 bmc/tls.hpp）。
// 未定义 BMC_TLS 时本文件是空实现：每个入口都抛 std::runtime_error，绝不静默降级成明文。
// 约束：握手与读写都不阻塞，WANT_READ/WANT_WRITE 经 events() 交回给 epoll 调度。
namespace bmc {
// 服务端上下文：TLS 最低 1.2，证书链与私钥必须同时可用且互相匹配（check_private_key），
// 任一环节失败都在构造期抛异常，避免带着半配置的上下文接收连接；同时显式关闭压缩。
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
// 释放 SSL_CTX；未启用 TLS 时 context_ 恒为空指针，无需释放。
TlsContext::~TlsContext() {
#ifdef BMC_TLS
    SSL_CTX_free(static_cast<SSL_CTX*>(context_));
#endif
}
// 客户端上下文：同样要求 TLS 1.2 起，并从 ca_file 载入受信 CA；SSL_VERIFY_PEER 启用证书链校验，
// 本文件不注册自定义校验回调。本文件刻意不提供“跳过验证”的开关。
// 仅校验证书链不足以确认对端身份：还要靠 TlsSession 里的主机名/IP 校验，否则任何由该 CA 签发的
// 证书都能冒充对端。
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
// 建立每连接的 SSL 状态：server_name 为空表示服务端（accept 状态），非空表示客户端（connect 状态）。
// 描述符归调用方所有，SSL 只借用它读写；underlying_ 用于继承 socket 层的系统调用。
// 初始 events_ = EPOLLIN：握手第一阶段是等待对端数据。
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
// 释放 SSL 对象；描述符仍归调用方，由网络层在连接结束时自行 close。
TlsSession::~TlsSession() {
#ifdef BMC_TLS
    SSL_free(static_cast<SSL*>(session_));
#endif
}
// 把 OpenSSL 的返回值翻译成 recv/send 的约定返回值：
// code == 1 表示成功，实际字节数由 count 给出（可能小于请求长度，调用方需自行续传），
// 并清空 events_，让调用方回退到按自身读写方向重新注册 epoll。
// WANT_READ/WANT_WRITE 不算失败：只把 errno 设为 EAGAIN，并把 events_ 换成匹配的 epoll 事件后返回 -1，
// 调用方据此注册 epoll，永远不必阻塞等待。
// 其余错误置 failed_：ZERO_RETURN 是对端干净关闭，返回 0；其它情况返回 -1 且 errno = EIO。
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
// 推进一次非阻塞握手：成功则 ready_ = true 并恢复 EPOLLIN；未完成返回 false，
// 该等可读还是可写由 result() 写进 events_，由调用方决定 epoll 关注方向。
bool TlsSession::handshake() {
#ifdef BMC_TLS
    const int code = SSL_do_handshake(static_cast<SSL*>(session_));
    if (code == 1) { ready_ = true; events_ = EPOLLIN; return true; }
    result(code, 0);
#endif
    return false;
}
// 只走 SSL_read_ex：fd 与 flags 参数被忽略（SSL 已绑定描述符，MSG_* 对 OpenSSL 无意义），
// 实际解密出的字节数经 result() 返回。
ssize_t TlsSession::recv(int, void* buffer, std::size_t count, int) {
#ifdef BMC_TLS
    std::size_t received = 0;
    const int code = SSL_read_ex(static_cast<SSL*>(session_), buffer, count, &received);
    return result(code, received);
#else
    static_cast<void>(buffer); static_cast<void>(count); errno = EIO; return -1;
#endif
}
// 只走 SSL_write_ex：一次调用可能只写出一部分，部分写入按成功返回实际字节数，
// 调用方必须自己续写剩余部分，不能假设一次 send 写完。
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
