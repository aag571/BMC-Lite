#include "bmc/control.hpp"
#include <algorithm>
#include <cerrno>
#include <array>
#include <cctype>
#include <fcntl.h>
#include <future>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

// 控制面：令牌文件校验、常量时间比较、两字段动作 JSON、失败限流与审计闸门都在本文件，
// 声明见 include/bmc/control.hpp。这是全进程唯一能触发硬件动作的 HTTP 入口，
// 因此认证、限流与审计在这里被串成一条无法绕开的路径。
namespace bmc {
// 读取并校验控制令牌文件：必须是当前有效用户拥有的普通文件、权限恰为 0640、
// O_NOFOLLOW 拒绝符号链接、内容为 32..512 个可打印非空格字符（允许一个结尾换行）。
// 任何一项不满足都抛出异常，绝不降级成"无令牌"或宽松模式。
std::string load_control_token(const std::string& path) {
    // O_NONBLOCK 避免在 FIFO 上卡住；打开之后再 fstat，校验的是真正打开的那个 inode。
    Fd descriptor(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat info{};
    if (descriptor.get() < 0 || ::fstat(descriptor.get(), &info) < 0 ||
        !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0777) != 0640 || info.st_size < 32 || info.st_size > 513)
        throw std::invalid_argument("token must be an owned regular file with mode 0640 and 32..512 characters");
    std::string token;
    // 上限 513 字节 = 512 字符 + 一个换行；缓冲再留一字节，便于发现超长文件。
    std::array<char, 4097> buffer{};
    for (;;) {
        const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("token read failed"); }
        if (count == 0) break;
        token.append(buffer.data(), static_cast<std::size_t>(count));
        if (token.size() > 513) throw std::invalid_argument("token too large");
    }
    // 只容忍一个结尾换行：CR、空格或第二个换行都会在下面的字符检查里失败。
    if (!token.empty() && token.back() == '\n') token.pop_back();
    // 留足 HTTP 单头部 1 KiB 的空间，不能接受实际上无法传输的超长凭证。
    if (token.size() < 32 || token.size() > 512 || !std::all_of(token.begin(), token.end(), [](unsigned char character) {
        return character >= 33 && character <= 126;
    })) throw std::invalid_argument("token must contain at least 32 printable non-space characters");
    return token;
}
// 常量时间比较：固定按服务端令牌长度循环，长度不同只置位 difference 而不提前返回，
// 因此耗时只与 expected 的长度相关，不泄漏任何前缀信息。
bool token_equal(const std::string& expected, const std::string& supplied) noexcept {
    // 固定扫描服务端令牌长度，内容差异不提前返回；长度本身不是秘密。
    volatile unsigned difference = expected.size() == supplied.size() ? 0u : 1u;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const unsigned byte = index < supplied.size() ? static_cast<unsigned char>(supplied[index]) : 0u;
        difference = difference | (static_cast<unsigned char>(expected[index]) ^ byte);
    }
    return difference == 0;
}
namespace {
// 控制协议只需要两个字符串字段，不引入通用 JSON 库。拒绝额外字段、重复键和尾随内容。
std::map<std::string, std::string> action_fields(const std::string& body) {
    std::size_t cursor = 0;
    // cursor 是唯一游标；space 跳过空白，punctuation 消费指定标点，text 只接受无转义字符串。
    auto space = [&] { while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor]))) ++cursor; };
    auto punctuation = [&](char wanted) {
        space();
        if (cursor >= body.size() || body[cursor++] != wanted) throw std::invalid_argument("invalid action JSON");
    };
    auto text = [&]() {
        punctuation('"');
        std::string value;
        while (cursor < body.size() && body[cursor] != '"') {
            const unsigned char character = static_cast<unsigned char>(body[cursor++]);
            // 控制字符、反斜杠与超长字符串都拒绝：这里不做转义解码，也不是通用 JSON。
            if (character < 32 || character == '\\' || value.size() >= 128)
                throw std::invalid_argument("unsupported action string");
            value += static_cast<char>(character);
        }
        punctuation('"');
        return value;
    };
    std::map<std::string, std::string> fields;
    punctuation('{');
    for (unsigned index = 0; index < 2; ++index) {
        const auto key = text();
        punctuation(':');
        const auto value = text();
        // emplace 的返回值顺带拒绝重复键；键名也只允许 sensor 与 action。
        if ((key != "sensor" && key != "action") || !fields.emplace(key, value).second)
            throw std::invalid_argument("only sensor and action are allowed");
        if (index == 0) punctuation(',');
    }
    punctuation('}'); space();
    // 右花括号之后必须恰好是输入末尾：任何尾随内容都判错。
    if (cursor != body.size()) throw std::invalid_argument("trailing JSON");
    return fields;
}
// 统一构造纯文本响应；消息固定补一个换行，Content-Length 由 HttpResponse::render() 补。
HttpResponse response(int status, const std::string& reason, const std::string& message) {
    return {status, reason, "text/plain", message + "\n"};
}
}
// 构造即校验：令牌长度与审计回调缺一不可，没有审计能力的控制面不允许存在。
ControlService::ControlService(std::string token, Worker& worker, RecoveryPolicyEngine& recovery, Audit audit)
    : token_(std::move(token)), worker_(worker), recovery_(recovery), audit_(std::move(audit)) {
    if (token_.size() < 32 || token_.size() > 512 || !audit_) throw std::invalid_argument("invalid control service");
}
// 替换"动作路径 + 代次"快照；调用方保证两者来自同一次配置加载。
void ControlService::configure(std::uint64_t generation, std::map<std::string, std::string> paths) {
    std::lock_guard lock(mutex_);
    paths_ = std::move(paths); generation_ = generation;
}
// 认证失败与限流计数，被只读 /metrics 汇总。
std::string ControlService::metrics() const {
    std::lock_guard lock(mutex_);
    return "bmc_control_auth_failures_total " + std::to_string(auth_failures_) + "\n" +
           "bmc_control_rate_limited_total " + std::to_string(rate_limited_) + "\n";
}
// 网络层的拒绝（连接数、解析失败、超时、断开）同样要留审计证据：
// 记录 action/sensor 为空、outcome=rejected，并带上原因与来源 IP。
void ControlService::reject(const std::string& peer, const std::string& reason) {
    std::lock_guard lock(mutex_);
    std::ostringstream record;
    record << "action=\"\" sensor=\"\" outcome=rejected peer=" << std::quoted(peer)
           << " request_id=" << ++sequence_ << " detail=" << std::quoted(reason);
    audit_(record.str());
}
// 控制请求的唯一入口，顺序固定：先校验令牌（失败先限流），再匹配路由，最后才解析请求体；
// 每一步失败都写审计并返回非 2xx。
// AUDIT GATE：会动硬件的任务先写 "requested" 审计，然后在 Worker 里等一个 promise；
// promise 只在 accepted/rejected 审计完成后置位，因此硬件动作不可能先于它的审计记录落盘。
HttpResponse ControlService::handle(const http::Request& request, const std::string& peer) {
    std::lock_guard lock(mutex_);
    const auto id = ++sequence_;
    std::string sensor, action;
    auto audit = [&](const std::string& outcome) {
        std::ostringstream record;
        record << "action=" << std::quoted(action) << " sensor=" << std::quoted(sensor)
               << " outcome=" << outcome << " peer=" << std::quoted(peer) << " request_id=" << id;
        audit_(record.str());
    };
    // 令牌先于任何路由判断：未认证的请求不应泄漏某个路由或传感器是否存在。
    if (!token_equal("Bearer " + token_, request.header("Authorization"))) {
        ++auth_failures_;
        // 限流键是来源 IP（peer 由网络层只取 inet_ntop 结果，不含端口），
        // 因此重连换源端口无法绕过计数。
        const bool allowed = failures_.allow(peer);
        if (!allowed) ++rate_limited_;
        audit(allowed ? "unauthorized" : "rate-limited");
        return response(allowed ? 401 : 429, allowed ? "Unauthorized" : "Too Many Requests", "authentication rejected");
    }
    // 唯一的只读路由：外部用它确认热重载后的配置代次。
    if (request.method == "GET" && request.target == "/v1/config/generation") {
        audit("accepted");
        return {200, "OK", "application/json", "{\"generation\": " + std::to_string(generation_) + "}\n"};
    }
    try {
        // 动作只接受 POST；目标、动作名与传感器必须同时匹配配置快照。
        if (request.method != "POST") throw std::invalid_argument("POST required");
        const auto fields = action_fields(request.body);
        const auto sensor_field = fields.find("sensor");
        const auto action_field = fields.find("action");
        if (sensor_field == fields.end() || action_field == fields.end())
            throw std::invalid_argument("missing action field");
        sensor = sensor_field->second; action = action_field->second;
        const bool fan = request.target == "/v1/actions/fan" && action == "increase_fan";
        const bool inspect = request.target == "/v1/actions/inspect" && action == "inspect_device";
        const auto config = paths_.find(sensor);
        if ((!fan && !inspect) || config == paths_.end() || (fan && config->second.empty()))
            throw std::invalid_argument("unknown sensor or unsupported action");
    } catch (const std::invalid_argument&) {
        audit("rejected");
        return response(400, "Bad Request", "invalid control request");
    }
    // 先持久化请求，再排队；闸门保证 accepted 审计完成前绝不执行硬件动作。
    audit("requested");
    const RecoveryRequest task{"control", sensor, action, id, paths_.at(sensor)};
    // 闸门：worker 侧先等 ready；主线程写完 accepted/rejected 审计之后才放行。
    auto gate = std::make_shared<std::promise<bool>>();
    const auto ready = gate->get_future().share();
    const auto completion = audit_;
    const auto audit_failed = audit_failed_;
    auto* recovery = &recovery_;
    bool queued = false;
    try {
        // 任务只捕获请求副本、审计回调与共享 future，不引用 ControlService 的成员。
        queued = worker_.submit([task, completion, recovery, peer, ready, audit_failed] {
            if (!ready.get()) return;
            const auto result = recovery->submit(task);
            std::ostringstream record;
            record << "action=" << std::quoted(task.action) << " sensor=" << std::quoted(task.sensor)
                   << " outcome=" << (result && result->success ? "accepted" : "rejected")
                   << " peer=" << std::quoted(peer) << " request_id=" << task.sequence
                   << " detail=" << std::quoted(result ? result->detail : "no result");
            try { completion(record.str()); }
            // 完成审计写失败时置位共享标志，主循环据此以非零码退出。
            catch (...) { audit_failed->store(true); throw; }
        }, 10);
        // 先落盘审计再放行任务：顺序不能颠倒，否则动作可能先于它的审计记录发生。
        audit(queued ? "accepted" : "rejected");
        gate->set_value(queued);
    } catch (...) {
        // 提交失败或审计抛出时同样要放闸，否则已入队的任务会在 ready.get() 上
        // 拿到 broken promise 异常：动作不会执行，却会被 Worker 记成一次失败任务。
        gate->set_value(false);
        throw;
    }
    // 202 表示已排队，503 表示恢复队列已满；两种结局都已经写过审计。
    return queued ? response(202, "Accepted", "queued")
                  : response(503, "Service Unavailable", "recovery queue full");
}
}
