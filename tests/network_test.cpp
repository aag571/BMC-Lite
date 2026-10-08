#include "bmc/network.hpp"
#include <gtest/gtest.h>
#include <cerrno>
#include <cstring>

// HttpConnection 的连接级状态机：部分写遇 EAGAIN 后续写、慢客户端的绝对超时，
// 以及解析失败时回 400 而不是把连接挂死。时间一律由用例注入 Clock，不依赖真实时长。
namespace {
// 极简的 SocketIo 替身：只驱动 HttpConnection 用到的 recv/send 两个方向。
//   - input：待读取的字节；recv 按需切分返回，读空后置 EAGAIN。
//   - output：send 累积的字节；每次最多写 7 字节，用来稳定复现部分写路径。
//   - blocked：把紧接着的一次 send 变成 EAGAIN。
// 不模拟：真实描述符（其余虚调用一律返回失败或成功占位）、accept/bind/listen 与时间推进。
class ScriptSocket final : public bmc::SocketIo {
public:
    std::string input, output;
    bool blocked = false;
    int socket(int, int, int) override { return -1; }
    int bind(int, const sockaddr*, socklen_t) override { return -1; }
    int listen(int, int) override { return -1; }
    int accept(int, sockaddr*, socklen_t*) override { return -1; }
    int setsockopt(int, int, int, const void*, socklen_t) override { return 0; }
    int getsockopt(int, int, int, void*, socklen_t*) override { return 0; }
    int shutdown(int, int) override { return 0; }
    ssize_t recv(int, void* buffer, std::size_t count, int) override {
        if (input.empty()) { errno = EAGAIN; return -1; }
        const auto size = std::min(count, input.size());
        std::memcpy(buffer, input.data(), size); input.erase(0, size);
        return static_cast<ssize_t>(size);
    }
    ssize_t send(int, const void* buffer, std::size_t count, int) override {
        if (blocked) { blocked = false; errno = EAGAIN; return -1; }
        const auto size = std::min<std::size_t>(7, count);
        output.append(static_cast<const char*>(buffer), size);
        return static_cast<ssize_t>(size);
    }
};
// 固定回 200 的处理器桩：让用例只关注连接状态机，不掺入任何路由逻辑。
bmc::HttpResponse ok(const bmc::http::Request&) { return {200, "OK", "text/plain", "hello"}; }
}
TEST(HttpConnection, PartialWritesResumeAfterEagain) {
    ScriptSocket io; io.input = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const auto now = bmc::HttpConnection::Clock::now();
    bmc::HttpConnection connection(123, io, ok, now);
    connection.read(now); ASSERT_TRUE(connection.writing());
    io.blocked = true; connection.write(); EXPECT_EQ(connection.sent(), 0u);
    for (unsigned index = 0; index < 100 && !connection.done(); ++index) connection.write();
    EXPECT_TRUE(connection.done()); EXPECT_EQ(io.output, ok({}).render());
}
TEST(HttpConnection, SlowClientHasAbsoluteDeadline) {
    ScriptSocket io; io.input = "GET / HTTP/1.1\r\nHost:";
    const auto now = bmc::HttpConnection::Clock::now();
    bmc::HttpConnection connection(123, io, ok, now);
    connection.read(now);
    EXPECT_FALSE(connection.expired(now + std::chrono::seconds(4)));
    EXPECT_TRUE(connection.expired(now + std::chrono::seconds(5)));
}
TEST(HttpConnection, ParseFailureProduces400) {
    ScriptSocket io; io.input = "invalid\r\n\r\n";
    const auto now = bmc::HttpConnection::Clock::now();
    bmc::HttpConnection connection(123, io, ok, now);
    connection.read(now); EXPECT_EQ(connection.status(), 400);
    for (unsigned index = 0; index < 100 && !connection.done(); ++index) connection.write();
    EXPECT_TRUE(connection.done()); EXPECT_NE(io.output.find("400 Bad Request"), std::string::npos);
}
