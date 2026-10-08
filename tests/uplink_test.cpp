#include "bmc/uplink.hpp"
#include "bmc/cli.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <cstring>

// Uplink 到汇聚端的连接管理：有界队列丢最旧、部分写续传、指数退避重连、pending connect
// 的 SO_ERROR 检查与绝对超时、空闲对端断开检测，以及单事件 JSON 行的编码与大小上限。
namespace {
// Uplink 用的 SocketIo 替身，围绕"连接状态"而非真实数据面设计：
//   - socket() 返回真实的 /dev/null 描述符，避免用例泄漏 fd。
//   - connect()：pending_connect 置 EINPROGRESS，error 非 0 时返回该 errno，用来分离握手与失败。
//   - writable() 为假表示握手未完成；getsockopt() 回填 error，模拟 SO_ERROR 的读数。
//   - recv()：eof 为真时返回 0（对端关闭），否则一律 EAGAIN（本用例不消费下行数据）。
//   - send()：默认每次最多写 7 字节并累积到 output；blocked 让下一次写返回 EAGAIN。
// 不模拟：真实网络往返、TLS 握手与真实描述符的读写语义。
class FakeUplinkSocket final : public bmc::SocketIo {
public:
    int connects = 0, error = 0;
    bool blocked = false, pending_connect = false, ready = true, eof = false;
    std::string output;
    int socket(int, int, int) override { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); }
    int connect(int, const sockaddr*, socklen_t) override {
        ++connects;
        if (pending_connect) { errno = EINPROGRESS; return -1; }
        if (error) { errno = error; return -1; }
        return 0;
    }
    int writable(int) override { return ready ? 1 : 0; }
    int bind(int, const sockaddr*, socklen_t) override { return 0; }
    int listen(int, int) override { return 0; }
    int accept(int, sockaddr*, socklen_t*) override { return -1; }
    int setsockopt(int, int, int, const void*, socklen_t) override { return 0; }
    int getsockopt(int, int, int, void* value, socklen_t*) override {
        *static_cast<int*>(value) = error; return 0;
    }
    ssize_t recv(int, void*, std::size_t, int) override { if (eof) return 0; errno = EAGAIN; return -1; }
    ssize_t send(int, const void* data, std::size_t size, int) override {
        if (blocked) { errno = EAGAIN; return -1; }
        const auto count = std::min<std::size_t>(size, 7);
        output.append(static_cast<const char*>(data), count);
        return static_cast<ssize_t>(count);
    }
    int shutdown(int, int) override { return 0; }
};
// 构造固定序列号 3 的 service 事件，供队列与编码断言复用。
bmc::BusEvent event(std::string message) { return {bmc::BusEventType::service, "test", std::move(message), std::nullopt, 3}; }
}
TEST(Uplink, BoundedQueueDropsOldestAndResumesPartialWrites) {
    FakeUplinkSocket io;
    bmc::Uplink uplink("127.0.0.1", 9000, 2, io);
    uplink.enqueue(event("old")); uplink.enqueue(event("second")); uplink.enqueue(event("new"));
    EXPECT_EQ(uplink.stats().queued, 2u); EXPECT_EQ(uplink.stats().dropped, 1u);
    const auto now = bmc::Uplink::Clock::now();
    io.blocked = true; uplink.advance(now);
    EXPECT_EQ(uplink.stats().sent, 0u);
    io.blocked = false;
    for (unsigned index = 0; index < 20; ++index) uplink.advance(now);
    EXPECT_EQ(uplink.stats().sent, 2u);
    EXPECT_EQ(io.output.find("\"message\": \"old\""), std::string::npos);
    EXPECT_NE(io.output.find("\"message\": \"new\""), std::string::npos);
    EXPECT_EQ(std::count(io.output.begin(), io.output.end(), '\n'), 2);
}
TEST(Uplink, ConnectFailureUsesExponentialBackoff) {
    FakeUplinkSocket io; io.error = ECONNREFUSED;
    bmc::Uplink uplink("127.0.0.1", 9000, 2, io);
    const auto now = bmc::Uplink::Clock::now();
    uplink.advance(now); uplink.advance(now + std::chrono::milliseconds(249));
    EXPECT_EQ(io.connects, 1);
    uplink.advance(now + std::chrono::milliseconds(250));
    uplink.advance(now + std::chrono::milliseconds(749));
    EXPECT_EQ(io.connects, 2);
    io.error = 0; uplink.enqueue(event("reconnected"));
    for (unsigned index = 0; index < 10; ++index) uplink.advance(now + std::chrono::milliseconds(750));
    EXPECT_EQ(io.connects, 3); EXPECT_EQ(uplink.stats().sent, 1u);
}
TEST(Uplink, SlowCollectorQueueAndInFlightStayBounded) {
    FakeUplinkSocket io; io.blocked = true;
    bmc::Uplink uplink("127.0.0.1", 9000, 4, io);
    const auto now = bmc::Uplink::Clock::now();
    uplink.enqueue(event("in-flight")); uplink.advance(now);
    for (unsigned index = 0; index < 50000; ++index) uplink.enqueue(event("load"));
    EXPECT_EQ(uplink.stats().queued, 4u); EXPECT_EQ(uplink.stats().dropped, 49996u);
    uplink.advance(now + std::chrono::seconds(5));
    EXPECT_FALSE(uplink.stats().connected);
    EXPECT_EQ(uplink.stats().dropped, 49997u);
    uplink.stop(); EXPECT_EQ(uplink.stats().queued, 0u);
    EXPECT_EQ(uplink.stats().dropped, 50001u);
}
TEST(Uplink, PendingConnectChecksErrorAndHasDeadline) {
    FakeUplinkSocket io; io.pending_connect = true; io.ready = false;
    bmc::Uplink uplink("127.0.0.1", 9000, 2, io);
    const auto now = bmc::Uplink::Clock::now();
    uplink.advance(now);
    EXPECT_FALSE(uplink.stats().connected);
    uplink.advance(now + std::chrono::seconds(5));
    EXPECT_FALSE(uplink.stats().connected);
    EXPECT_EQ(uplink.stats().attempts, 1u);
    io.ready = true; io.error = ECONNREFUSED;
    uplink.advance(now + std::chrono::seconds(6));
    EXPECT_FALSE(uplink.stats().connected);
}
TEST(Uplink, OversizedEventAndInvalidConfigurationAreRejected) {
    FakeUplinkSocket io; bmc::Uplink uplink("127.0.0.1", 9000, 2, io);
    EXPECT_FALSE(uplink.enqueue(event(std::string(4097, 'x'))));
    EXPECT_EQ(uplink.stats().dropped, 1u);
    EXPECT_THROW(bmc::Uplink("invalid", 9000), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--uplink-address", "127.0.0.1"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--uplink-port", "70000"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--uplink-capacity", "4097"}), std::invalid_argument);
    for (const auto* flag : {"--uplink-address", "--uplink-port", "--uplink-capacity"})
        EXPECT_NE(bmc::usage().find(flag), std::string::npos);
}
// 保活不能只在"有事件要发"时才做：队列为空时也要探测对端关闭（recv 返回 0）。
TEST(Uplink, IdlePeerDisconnectIsDetectedWithoutNewEvents) {
    FakeUplinkSocket io;
    bmc::Uplink uplink("127.0.0.1", 9000, 2, io);
    const auto now = bmc::Uplink::Clock::now();
    uplink.advance(now); EXPECT_TRUE(uplink.stats().connected);
    io.eof = true; uplink.advance(now); EXPECT_FALSE(uplink.stats().connected);
    io.eof = false; uplink.advance(now + std::chrono::milliseconds(250));
    EXPECT_TRUE(uplink.stats().connected); EXPECT_EQ(io.connects, 2);
}
TEST(Uplink, EveryBusTypeUsesOneBoundedJsonLine) {
    FakeUplinkSocket io; bmc::Uplink uplink("127.0.0.1", 9000, 4, io);
    for (const auto type : {bmc::BusEventType::sensor_state, bmc::BusEventType::configuration,
                           bmc::BusEventType::service, bmc::BusEventType::recovery})
        uplink.enqueue({type, "cpu", "quote\" and newline\n", 12.5, 7});
    for (unsigned index = 0; index < 20; ++index) uplink.advance(bmc::Uplink::Clock::now());
    EXPECT_EQ(uplink.stats().sent, 4u);
    EXPECT_EQ(std::count(io.output.begin(), io.output.end(), '\n'), 4);
    for (const auto* type : {"sensor_state", "configuration", "service", "recovery"})
        EXPECT_NE(io.output.find(type), std::string::npos);
}
