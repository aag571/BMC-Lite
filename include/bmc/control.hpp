#pragma once
// 控制面：令牌文件校验、常量时间比较，以及 POST 动作端点的鉴权、审计与限流。
// 实现见 src/control.cpp。
#include "bmc/core.hpp"
#include "bmc/http.hpp"
#include "bmc/service.hpp"
#include <atomic>
#include <map>
#include <mutex>

namespace bmc {
// 令牌读取只接受服务账户拥有的普通文件；不跟随符号链接。
std::string load_control_token(const std::string& path);
// 常量时间比较：始终扫完 expected 的全部字节，内容差异不提前返回，避免计时侧信道。
// 长度不同直接判否（长度本身不是秘密）。不抛异常。
bool token_equal(const std::string& expected, const std::string& supplied) noexcept;

// 网络层只能访问这个配置快照，不能访问 MonitorSensor。
// 审计失败标志用 shared_ptr 持有，网络线程无需加锁即可读取 failed()。
class ControlService {
public:
    using Audit = std::function<void(const std::string&)>;
    // 令牌长度须在 32..512 之间且 audit 非空，否则抛出 std::invalid_argument。
    ControlService(std::string token, Worker& worker, RecoveryPolicyEngine& recovery, Audit audit);
    // 热加载后整体替换动作路径表并更新代次，加锁进行，可在网络线程之外调用。
    void configure(std::uint64_t generation, std::map<std::string, std::string> paths);
    // 鉴权、限流、审计与派发都在锁内完成，因此请求被串行处理；
    // 请求格式非法（字段缺失、尾随内容、动作不支持）抛出 std::invalid_argument。
    HttpResponse handle(const http::Request& request, const std::string& peer);
    // 记录一条被拒绝的审计行（含原因），不返回响应；调用方负责生成状态码。
    void reject(const std::string& peer, const std::string& reason);
    std::string metrics() const;
    bool failed() const noexcept { return audit_failed_->load(); }
private:
    std::string token_;
    Worker& worker_;
    RecoveryPolicyEngine& recovery_;
    Audit audit_;
    std::shared_ptr<std::atomic_bool> audit_failed_ = std::make_shared<std::atomic_bool>(false);
    http::RateLimiter failures_{5, 0.2};
    mutable std::mutex mutex_;
    std::uint64_t auth_failures_ = 0, rate_limited_ = 0;
    std::map<std::string, std::string> paths_;
    std::uint64_t generation_ = 1, sequence_ = 0;
};
}
