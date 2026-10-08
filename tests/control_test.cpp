#include "bmc/control.hpp"
#include "bmc/network.hpp"
#include "bmc/cli.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

// 控制面的安全边界与行为：令牌文件的权限/符号链接/长度校验、常量时间比较、非回环明文监听
// 必须在建立套接字前失败，以及 ControlService 的鉴权、限流、路径白名单与审计失败语义。
TEST(Control, TokenFileRejectsPermissionsAndSymlinks) {
    const auto path = "/tmp/bmc-token-" + std::to_string(::getpid());
    { std::ofstream file(path); file << std::string(40, 'x') << '\n'; }
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    EXPECT_THROW(bmc::load_control_token(path), std::invalid_argument);
    ASSERT_EQ(::chmod(path.c_str(), 0640), 0);
    EXPECT_EQ(bmc::load_control_token(path), std::string(40, 'x'));
    const auto link = path + ".link";
    ASSERT_EQ(::symlink(path.c_str(), link.c_str()), 0);
    EXPECT_THROW(bmc::load_control_token(link), std::invalid_argument);
    ::unlink(link.c_str()); ::unlink(path.c_str());
}
TEST(Control, ConstantTimeComparisonChecksContentAndLength) {
    EXPECT_TRUE(bmc::token_equal("abc", "abc"));
    EXPECT_FALSE(bmc::token_equal("abc", "abd"));
    EXPECT_FALSE(bmc::token_equal("abc", "ab"));
    EXPECT_FALSE(bmc::token_equal("abc", "abcd"));
}
TEST(Control, TokenLengthFitsHttpHeaderLimit) {
    const auto path = "/tmp/bmc-token-length-" + std::to_string(::getpid());
    { std::ofstream output(path); output << std::string(512, 'x') << '\n'; }
    ASSERT_EQ(::chmod(path.c_str(), 0640), 0);
    EXPECT_EQ(bmc::load_control_token(path).size(), 512u);
    { std::ofstream output(path); output << std::string(513, 'x'); }
    EXPECT_THROW(bmc::load_control_token(path), std::invalid_argument);
    ::unlink(path.c_str());
}
TEST(Control, NonLoopbackPlainListenerFailsBeforeSocketCreation) {
    bmc::ReadOnlyServer server("0.0.0.0", 8443,
        [](const auto&, const auto&) { return bmc::HttpResponse{}; },
        [](const auto&, const auto&) {}, "", "");
    std::string error;
    EXPECT_FALSE(server.start(error));
    EXPECT_NE(error.find("requires TLS"), std::string::npos);
}
TEST(Peer, NonLoopbackPlainListenerFailsBeforeSocketCreation) {
    bmc::ReadOnlyServer server("0.0.0.0", 9443,
        [](const auto&, const auto&) { return bmc::HttpResponse{}; },
        {}, "", "");
    server.role("peer");
    std::string error;
    EXPECT_FALSE(server.start(error));
    EXPECT_NE(error.find("non-loopback peer requires TLS"), std::string::npos);
}
TEST(Control, CliDocumentsAndParsesControlOptions) {
    const auto options = bmc::parse_options({"--control-port", "8443", "--control-token-file", "/token",
        "--control-bind", "0.0.0.0", "--control-tls-cert", "/cert", "--control-tls-key", "/key"});
    EXPECT_EQ(options.control_port, 8443u);
    EXPECT_EQ(options.control_token_file, "/token");
    for (const auto* flag : {"--control-port", "--control-token-file", "--control-bind", "--control-tls-cert", "--control-tls-key"})
        EXPECT_NE(bmc::usage().find(flag), std::string::npos);
    EXPECT_THROW(bmc::parse_options({"--control-port", "65536"}), std::invalid_argument);
    EXPECT_THROW(bmc::parse_options({"--http-port", "8000", "--http-bind", "0.0.0.0"}), std::invalid_argument);
}
TEST(Control, StrictParserRejectsAmbiguousAndUnsupportedHeaders) {
    for (const auto& headers : {
        "Content-Length: 0\r\nContent-Length: 0\r\n",
        "Authorization: Bearer one\r\nAuthorization: Bearer two\r\n"}) {
        bmc::http::Parser parser;
        EXPECT_EQ(parser.feed(std::string("POST / HTTP/1.1\r\n") + headers + "\r\n"), bmc::http::ParseResult::malformed);
    }
    for (const auto& headers : {"Expect: 100-continue\r\n", "Upgrade: websocket\r\n"}) {
        bmc::http::Parser parser;
        EXPECT_EQ(parser.feed(std::string("POST / HTTP/1.1\r\n") + headers + "\r\n"), bmc::http::ParseResult::unsupported);
    }
}
// ============ ControlService：鉴权、限流、路径白名单与审计 ============
TEST(Control, AuthenticationRateLimitAndPathRejectionAreAudited) {
    bmc::Worker worker(8, 1);
    bmc::RecoveryPolicyEngine recovery([](const bmc::RecoveryRequest&) { return true; });
    std::vector<std::string> audit;
    bmc::ControlService service(std::string(40, 'x'), worker, recovery, [&](const auto& record) { audit.push_back(record); });
    service.configure(7, {{"cpu", "/configured/pwm"}});
    bmc::http::Request request; request.method = "GET"; request.target = "/v1/config/generation";
    for (unsigned index = 0; index < 5; ++index) EXPECT_EQ(service.handle(request, "127.0.0.1").status, 401);
    EXPECT_EQ(service.handle(request, "127.0.0.1").status, 429);
    request.headers.emplace_back("Authorization", "Bearer " + std::string(40, 'x'));
    EXPECT_EQ(service.handle(request, "127.0.0.1").body, "{\"generation\": 7}\n");
    request.method = "POST"; request.target = "/v1/actions/fan";
    request.body = "{\"sensor\":\"cpu\",\"path\":\"/tmp/evil\"}";
    EXPECT_EQ(service.handle(request, "127.0.0.1").status, 400);
    EXPECT_NE(audit.back().find("outcome=rejected"), std::string::npos);
}
TEST(Control, WorkerUsesConfiguredPathAndRecoveryCooldown) {
    bmc::Worker worker(8, 1);
    std::atomic<unsigned> writes{0};
    bmc::RecoveryPolicyEngine recovery([&](const bmc::RecoveryRequest& request) {
        EXPECT_EQ(request.path, "/configured/pwm");
        ++writes;
        return true;
    });
    bmc::ControlService service(std::string(40, 'x'), worker, recovery, [](const auto&) {});
    service.configure(1, {{"cpu", "/configured/pwm"}});
    bmc::http::Request request;
    request.method = "POST"; request.target = "/v1/actions/fan";
    request.headers.emplace_back("Authorization", "Bearer " + std::string(40, 'x'));
    request.body = "{\"sensor\":\"cpu\",\"action\":\"increase_fan\"}";
    EXPECT_EQ(service.handle(request, "127.0.0.1").status, 202);
    EXPECT_EQ(service.handle(request, "127.0.0.1").status, 202);
    worker.stop();
    EXPECT_EQ(writes.load(), 1u);
}
// 审计 "accepted" 写入失败时动作不得入队：顺序是先审计后提交，异常直接上抛。
TEST(Control, AuditFailureCannotExecuteQueuedAction) {
    bmc::Worker worker(8, 1);
    std::atomic<unsigned> writes{0};
    bmc::RecoveryPolicyEngine recovery([&](const bmc::RecoveryRequest&) {
        ++writes;
        return true;
    });
    std::mutex audit_mutex;
    std::vector<std::string> records;
    bmc::ControlService service(std::string(40, 'x'), worker, recovery, [&](const auto& record) {
        std::lock_guard lock(audit_mutex);
        records.push_back(record);
        if (record.find("outcome=accepted") != std::string::npos)
            throw std::runtime_error("audit unavailable");
    });
    service.configure(1, {{"cpu", "/configured/pwm"}});
    bmc::http::Request request;
    request.method = "POST"; request.target = "/v1/actions/fan";
    request.headers.emplace_back("Authorization", "Bearer " + std::string(40, 'x'));
    request.body = "{\"sensor\":\"cpu\",\"action\":\"increase_fan\"}";
    EXPECT_THROW(service.handle(request, "127.0.0.1"), std::runtime_error);
    worker.stop();
    EXPECT_EQ(writes.load(), 0u);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_NE(records[0].find("outcome=requested"), std::string::npos);
    EXPECT_NE(records[1].find("outcome=accepted"), std::string::npos);
}
// 完成态审计在 worker 线程里失败：由 service.failed() 与 worker 失败计数暴露，不静默吞掉。
TEST(Control, CompletionAuditFailureIsVisibleToDaemon) {
    bmc::Worker worker(8, 1);
    bmc::RecoveryPolicyEngine recovery([](const bmc::RecoveryRequest&) { return true; });
    bmc::ControlService service(std::string(40, 'x'), worker, recovery, [](const auto& record) {
        if (record.find("detail=") != std::string::npos)
            throw std::runtime_error("completion audit unavailable");
    });
    service.configure(1, {{"cpu", "/configured/pwm"}});
    bmc::http::Request request;
    request.method = "POST"; request.target = "/v1/actions/fan";
    request.headers.emplace_back("Authorization", "Bearer " + std::string(40, 'x'));
    request.body = "{\"sensor\":\"cpu\",\"action\":\"increase_fan\"}";
    EXPECT_EQ(service.handle(request, "127.0.0.1").status, 202);
    worker.stop();
    EXPECT_TRUE(service.failed());
    EXPECT_EQ(worker.stats().failed, 1u);
}
