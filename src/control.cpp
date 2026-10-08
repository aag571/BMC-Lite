#include "bmc/control.hpp"
#include <algorithm>
#include <cerrno>
#include <array>
#include <cctype>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace bmc {
std::string load_control_token(const std::string& path) {
    Fd descriptor(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat info{};
    if (descriptor.get() < 0 || ::fstat(descriptor.get(), &info) < 0 ||
        !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0777) != 0640 || info.st_size < 32 || info.st_size > 513)
        throw std::invalid_argument("token must be an owned regular file with mode 0640 and 32..512 characters");
    std::string token;
    std::array<char, 4097> buffer{};
    for (;;) {
        const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error("token read failed"); }
        if (count == 0) break;
        token.append(buffer.data(), static_cast<std::size_t>(count));
        if (token.size() > 513) throw std::invalid_argument("token too large");
    }
    if (!token.empty() && token.back() == '\n') token.pop_back();
    // 留足 HTTP 单头部 1 KiB 的空间，不能接受实际上无法传输的超长凭证。
    if (token.size() < 32 || token.size() > 512 || !std::all_of(token.begin(), token.end(), [](unsigned char character) {
        return character >= 33 && character <= 126;
    })) throw std::invalid_argument("token must contain at least 32 printable non-space characters");
    return token;
}
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
        if ((key != "sensor" && key != "action") || !fields.emplace(key, value).second)
            throw std::invalid_argument("only sensor and action are allowed");
        if (index == 0) punctuation(',');
    }
    punctuation('}'); space();
    if (cursor != body.size()) throw std::invalid_argument("trailing JSON");
    return fields;
}
HttpResponse response(int status, const std::string& reason, const std::string& message) {
    return {status, reason, "text/plain", message + "\n"};
}
}
ControlService::ControlService(std::string token, Worker& worker, RecoveryPolicyEngine& recovery, Audit audit)
    : token_(std::move(token)), worker_(worker), recovery_(recovery), audit_(std::move(audit)) {
    if (token_.size() < 32 || token_.size() > 512 || !audit_) throw std::invalid_argument("invalid control service");
}
void ControlService::configure(std::uint64_t generation, std::map<std::string, std::string> paths) {
    std::lock_guard lock(mutex_);
    paths_ = std::move(paths); generation_ = generation;
}
std::string ControlService::metrics() const {
    std::lock_guard lock(mutex_);
    return "bmc_control_auth_failures_total " + std::to_string(auth_failures_) + "\n" +
           "bmc_control_rate_limited_total " + std::to_string(rate_limited_) + "\n";
}
void ControlService::reject(const std::string& peer, const std::string& reason) {
    std::lock_guard lock(mutex_);
    std::ostringstream record;
    record << "action=\"\" sensor=\"\" outcome=rejected peer=" << std::quoted(peer)
           << " request_id=" << ++sequence_ << " detail=" << std::quoted(reason);
    audit_(record.str());
}
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
    if (!token_equal("Bearer " + token_, request.header("Authorization"))) {
        ++auth_failures_;
        const bool allowed = failures_.allow(peer);
        if (!allowed) ++rate_limited_;
        audit(allowed ? "unauthorized" : "rate-limited");
        return response(allowed ? 401 : 429, allowed ? "Unauthorized" : "Too Many Requests", "authentication rejected");
    }
    if (request.method == "GET" && request.target == "/v1/config/generation") {
        audit("accepted");
        return {200, "OK", "application/json", "{\"generation\": " + std::to_string(generation_) + "}\n"};
    }
    try {
        if (request.method != "POST") throw std::invalid_argument("POST required");
        const auto fields = action_fields(request.body);
        sensor = fields.at("sensor"); action = fields.at("action");
        const bool fan = request.target == "/v1/actions/fan" && action == "increase_fan";
        const bool inspect = request.target == "/v1/actions/inspect" && action == "inspect_device";
        const auto config = paths_.find(sensor);
        if ((!fan && !inspect) || config == paths_.end() || (fan && config->second.empty()))
            throw std::invalid_argument("unknown sensor or unsupported action");
        const RecoveryRequest task{"control", sensor, action, id, config->second};
        // 先确保请求审计写入成功，再允许任务进入线程池。
        audit("accepted");
        const auto completion = audit_;
        auto* recovery = &recovery_;
        if (!worker_.submit([task, completion, recovery, peer] {
            const auto result = recovery->submit(task);
            std::ostringstream record;
            record << "action=" << std::quoted(task.action) << " sensor=" << std::quoted(task.sensor)
                   << " outcome=" << (result && result->success ? "accepted" : "rejected")
                   << " peer=" << std::quoted(peer) << " request_id=" << task.sequence
                   << " detail=" << std::quoted(result ? result->detail : "no result");
            completion(record.str());
        }, 10)) {
            audit("rejected");
            return response(503, "Service Unavailable", "recovery queue full");
        }
        return response(202, "Accepted", "queued");
    } catch (const std::invalid_argument&) {
        audit("rejected");
        return response(400, "Bad Request", "invalid control request");
    }
}
}
