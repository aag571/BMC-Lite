#pragma once
// HTTP/1.1 严格子集的解析与限流：硬上限、增量请求解析器、令牌桶。
// 实现见 src/http.cpp。
#include "bmc/socket_io.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bmc {
// HTTP/1.1 的严格子集。刻意做小：不支持 chunked、不支持 keep-alive 复用、
// 不支持 GET/POST 以外的方法，因此连接状态机可以完全在单测里覆盖。
namespace http {
// 硬上限。超出即拒绝并计数，不做静默截断。
constexpr std::size_t kMaxRequestLine = 8 * 1024;
constexpr std::size_t kMaxHeaderTotal = 8 * 1024;
constexpr std::size_t kMaxHeaderLine = 1024;
constexpr std::size_t kMaxHeaders = 64;
constexpr std::size_t kMaxBody = 1024 * 1024;

enum class ParseResult {
    incomplete,   // 需要更多字节
    complete,     // request() 可用，调用方处理后应 reset()
    malformed,    // 语法错误
    too_large,    // 超过硬上限
    unsupported,  // 语法合法但不支持（如 chunked）
};

// 一条已完整解析的请求。headers 保持原始顺序，便于逐字回显与审计。
struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // 头部名大小写不敏感；缺失返回空串。
    std::string header(const std::string& name) const;
    bool has_header(const std::string& name) const;
};

// 增量解析器：同一个连接上反复 feed()，返回 complete 时取 request() 再 reset()。
class Parser {
public:
    ParseResult feed(const char* data, std::size_t size);
    ParseResult feed(const std::string& data) { return feed(data.data(), data.size()); }
    const Request& request() const { return request_; }
    // 当前仍未被消费的字节数（尚未构成完整请求的部分）。
    std::size_t buffered_bytes() const { return buffered_.size(); }
    void reset();

private:
    // 返回 true 表示已产出一条完整请求（或已判定为错误，error_ 已置位）。
    ParseResult progress();

    std::string buffered_;
    Request request_;
    ParseResult error_ = ParseResult::incomplete;
    // 请求行是否已解析。不能用 request_.method.empty() 代替：method 在 complete 之后
    // 仍保留给调用方读取，用它判断会让同一连接上的下一条请求跳过请求行解析。
    bool have_request_line_ = false;
    bool have_headers_ = false;
    // 上一条请求已完整产出。下一次 feed() 会先清空解析状态，从而开始解析新请求。
    bool finished_ = false;
    std::size_t body_length_ = 0;
    std::size_t header_bytes_ = 0;
};

// 令牌桶限流。控制/心跳以来源 IP 为键；键表满时回收最旧项，避免永久拒绝新地址。
class RateLimiter {
public:
    RateLimiter(double capacity, double refill_per_second, std::size_t max_keys = 1024);

    // 消耗一个令牌；额度不足返回 false。
    bool allow(const std::string& key, std::chrono::steady_clock::time_point now);
    bool allow(const std::string& key) { return allow(key, std::chrono::steady_clock::now()); }
    // 只读查询，不消耗令牌。
    double tokens(const std::string& key, std::chrono::steady_clock::time_point now) const;
    std::size_t tracked_keys() const { return buckets_.size(); }
    void clear() { buckets_.clear(); }

private:
    struct Bucket { double tokens = 0; std::chrono::steady_clock::time_point stamp; };
    double capacity_;
    double refill_;
    std::size_t max_keys_;
    std::unordered_map<std::string, Bucket> buckets_;
};
}
}
