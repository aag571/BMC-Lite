#include "bmc/uplink.hpp"
#include "bmc/cli.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <cstring>

namespace {
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
