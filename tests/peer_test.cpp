#include "bmc/peer.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <fcntl.h>

namespace {
class PeerSocket final : public bmc::SocketIo {
public:
    std::string input, output;
    bool block = false, pending = false, eof = false;
    int connects = 0;
    int socket(int, int, int) override { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); }
    int bind(int, const sockaddr*, socklen_t) override { return 0; }
    int listen(int, int) override { return 0; }
    int accept(int, sockaddr*, socklen_t*) override { errno = EAGAIN; return -1; }
    int connect(int, const sockaddr*, socklen_t) override { ++connects; if (pending) { errno = EINPROGRESS; return -1; } return 0; }
    int writable(int) override { return pending ? 0 : 1; }
    int setsockopt(int, int, int, const void*, socklen_t) override { return 0; }
    int getsockopt(int, int, int, void* value, socklen_t*) override { *static_cast<int*>(value) = 0; return 0; }
    int shutdown(int, int) override { return 0; }
    ssize_t send(int, const void* data, std::size_t count, int) override {
        if (block) { errno = EAGAIN; return -1; }
        const auto size = std::min<std::size_t>(count, 7);
        output.append(static_cast<const char*>(data), size); return static_cast<ssize_t>(size);
    }
    ssize_t recv(int, void* data, std::size_t count, int) override {
        if (input.empty()) { if (eof) return 0; errno = EAGAIN; return -1; }
        const auto size = std::min(count, input.size());
        std::memcpy(data, input.data(), size); input.erase(0, size); return static_cast<ssize_t>(size);
    }
};
bmc::http::Request heartbeat(std::string token = std::string(48, 'x')) {
    return {"GET", "/v1/heartbeat", "HTTP/1.1", {{"Authorization", "Bearer " + token}, {"X-BMC-Generation", "7"}}, {}};
}
}
TEST(PeerHeartbeat, AuthenticatedGenerationOnly) {
    PeerSocket io;
    bmc::PeerHeartbeat peer("127.0.0.1", 1234, std::string(48, 'x'), std::chrono::milliseconds(100),
        std::chrono::milliseconds(500), [](const auto&) {}, {}, {}, io);
    peer.generation(3);
    EXPECT_EQ(peer.handle(heartbeat(), "127.0.0.1").body, "3\n");
    EXPECT_NE(peer.metrics().find("bmc_peer_generation 7\n"), std::string::npos);
    auto action = heartbeat(); action.target = "/v1/actions/fan";
    EXPECT_EQ(peer.handle(action, "127.0.0.1").status, 400);
    action = heartbeat(); action.headers[1].second = "18446744073709551616";
    EXPECT_EQ(peer.handle(action, "127.0.0.1").status, 400);
    for (int index = 0; index < 5; ++index) EXPECT_EQ(peer.handle(heartbeat("wrong"), "127.0.0.1").status, 401);
    EXPECT_EQ(peer.handle(heartbeat("wrong"), "127.0.0.1").status, 429);
}
TEST(PeerHeartbeat, StaleTransitionAndRecoveryAreReportedOnce) {
    PeerSocket io; io.pending = true;
    std::vector<std::string> reports;
    bmc::PeerHeartbeat peer("127.0.0.1", 1234, std::string(48, 'x'), std::chrono::milliseconds(100),
        std::chrono::milliseconds(500), [&](const auto& message) { reports.push_back(message); }, {}, {}, io);
    auto now = bmc::PeerHeartbeat::Clock::now() + std::chrono::seconds(1);
    peer.advance(now); peer.advance(now + std::chrono::seconds(1));
    ASSERT_EQ(reports.size(), 1u); EXPECT_NE(reports[0].find("stale"), std::string::npos);
    peer.handle(heartbeat(), "127.0.0.1");
    ASSERT_EQ(reports.size(), 2u); EXPECT_NE(reports[1].find("recovered"), std::string::npos);
}
TEST(PeerHeartbeat, PartialWriteEagainAndBoundedResponse) {
    PeerSocket io;
    io.input = bmc::HttpResponse{200, "OK", "text/plain", "9\n"}.render();
    bmc::PeerHeartbeat peer("127.0.0.1", 1234, std::string(48, 'x'), std::chrono::milliseconds(100),
        std::chrono::milliseconds(500), [](const auto&) {}, {}, {}, io);
    const auto now = bmc::PeerHeartbeat::Clock::now();
    io.block = true; peer.advance(now); EXPECT_TRUE(io.output.empty());
    io.block = false;
    for (int index = 0; index < 100; ++index) peer.advance(now);
    EXPECT_EQ(io.connects, 1); EXPECT_NE(io.output.find("X-BMC-Generation: 1"), std::string::npos);
    EXPECT_NE(peer.metrics().find("bmc_peer_generation 9\n"), std::string::npos);
    io.input.assign(9000, 'a');
    for (int index = 0; index < 100; ++index) peer.advance(now + std::chrono::milliseconds(100));
    EXPECT_NE(peer.metrics().find("bmc_peer_failures_total 1\n"), std::string::npos);
}
TEST(PeerHeartbeat, PendingConnectionUsesAbsoluteDeadlineAndRetryInterval) {
    PeerSocket io; io.pending = true;
    bmc::PeerHeartbeat peer("127.0.0.1", 1234, std::string(48, 'x'), std::chrono::milliseconds(100),
        std::chrono::milliseconds(500), [](const auto&) {}, {}, {}, io);
    const auto now = bmc::PeerHeartbeat::Clock::now();
    peer.advance(now); peer.advance(now + std::chrono::milliseconds(500));
    EXPECT_EQ(io.connects, 1);
    peer.advance(now + std::chrono::milliseconds(599)); EXPECT_EQ(io.connects, 1);
    peer.advance(now + std::chrono::milliseconds(600)); EXPECT_EQ(io.connects, 2);
}
TEST(PeerHeartbeat, PlaintextRemoteAndIncompleteTlsAreRejected) {
    EXPECT_THROW(bmc::PeerHeartbeat("192.168.124.128", 1234, std::string(48, 'x'),
        std::chrono::milliseconds(100), std::chrono::milliseconds(500), [](const auto&) {}), std::invalid_argument);
    EXPECT_THROW(bmc::PeerHeartbeat("127.0.0.1", 1234, std::string(48, 'x'),
        std::chrono::milliseconds(100), std::chrono::milliseconds(500), [](const auto&) {}, "ca", ""), std::invalid_argument);
}
