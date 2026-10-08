#pragma once
#include "bmc/core.hpp"
#include "bmc/http.hpp"
#include "bmc/service.hpp"
#include <map>
#include <mutex>

namespace bmc {
// 令牌读取只接受服务账户拥有的普通文件；不跟随符号链接。
std::string load_control_token(const std::string& path);
bool token_equal(const std::string& expected, const std::string& supplied) noexcept;

// 网络层只能访问这个配置快照，不能访问 MonitorSensor。
class ControlService {
public:
    using Audit = std::function<void(const std::string&)>;
    ControlService(std::string token, Worker& worker, RecoveryPolicyEngine& recovery, Audit audit);
    void configure(std::uint64_t generation, std::map<std::string, std::string> paths);
    HttpResponse handle(const http::Request& request, const std::string& peer);
    void reject(const std::string& peer, const std::string& reason);
    std::string metrics() const;
private:
    std::string token_;
    Worker& worker_;
    RecoveryPolicyEngine& recovery_;
    Audit audit_;
    http::RateLimiter failures_{5, 0.2};
    mutable std::mutex mutex_;
    std::uint64_t auth_failures_ = 0, rate_limited_ = 0;
    std::map<std::string, std::string> paths_;
    std::uint64_t generation_ = 1, sequence_ = 0;
};
}
