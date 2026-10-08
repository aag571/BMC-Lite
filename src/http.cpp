#include "bmc/http.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace bmc {
// HTTP/1.1 严格子集：Request、Parser 与 RateLimiter 定义在本文件，声明见 include/bmc/http.hpp。
// 解析器不持有 socket，只按字节增量推进，因此网络线程与单测共用同一份状态机；
// 所有上限都是硬拒绝（too_large / malformed），绝不静默截断。
namespace http {
namespace {
// 头部名与头部查找都按 ASCII 小写折叠，调用方只对折叠后的结果做等值比较。
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}
bool is_token_character(unsigned char character) {
    // RFC 7230 token：可见 ASCII，排除分隔符。
    if (character <= 0x20 || character >= 0x7f) {
        return false;
    }
    switch (character) {
    case '"': case '(': case ')': case ',': case '/': case ':': case ';': case '<':
    case '=': case '>': case '?': case '@': case '[': case '\\': case ']': case '{':
    case '}':
        return false;
    default:
        return true;
    }
}
// 解析 Content-Length；非法或溢出返回 false。
bool parse_length(const std::string& text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    std::size_t result = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::size_t>(character - '0');
        // 乘 10 之前先判溢出：否则超长数字会回绕成小值，绕过 kMaxBody 检查。
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    value = result;
    return true;
}
}

// 头部查找大小写不敏感；同名头部返回第一个，未找到返回空串。
std::string Request::header(const std::string& name) const {
    const auto wanted = lower(name);
    for (const auto& [key, value] : headers) {
        if (lower(key) == wanted) {
            return value;
        }
    }
    return {};
}

// 只判断存在性，不取值；用于把 Transfer-Encoding 这类头部整体判为不支持。
bool Request::has_header(const std::string& name) const {
    const auto wanted = lower(name);
    return std::any_of(headers.begin(), headers.end(),
        [&wanted](const auto& entry) { return lower(entry.first) == wanted; });
}

// 清空缓冲区与全部阶段标志，并清掉粘性错误，使同一个 Parser 可以在新连接上复用。
void Parser::reset() {
    buffered_.clear();
    request_ = Request{};
    error_ = ParseResult::incomplete;
    have_request_line_ = false;
    have_headers_ = false;
    finished_ = false;
    body_length_ = 0;
    header_bytes_ = 0;
}

// 增量入口：追加字节并按需推进状态机。错误是粘性的——一旦返回 malformed/too_large/
// unsupported，后续 feed 只会重复该结果，直到调用方 reset()。
// 上限在 append 之前检查：超限直接判错，既不截断，也不让缓冲区无界增长。
ParseResult Parser::feed(const char* data, std::size_t size) {
    if (error_ != ParseResult::incomplete) {
        // 上一轮已判定错误：保持错误状态，调用方应 reset() 后复用。
        return error_;
    }
    if (data == nullptr || size == 0) {
        return ParseResult::incomplete;
    }
    // 上一条请求已结束，调用方现在送来了新字节：开始解析下一条请求。
    // request_ 里的上一条结果此时已被调用方读过（或本连接已关闭），可以安全清空。
    // 缺少这一步会让两个阶段标志一直保持为真，于是之后每次 feed 都只会重复返回
    // 上一条请求的 complete，并把新数据当成它的请求体。
    if (finished_) {
        request_ = Request{};
        have_request_line_ = false;
        have_headers_ = false;
        body_length_ = 0;
        header_bytes_ = 0;
        finished_ = false;
    }
    // 请求头尚未结束时，先在累计前拦截超限，避免无界增长。
    if (!have_headers_) {
        if (buffered_.size() + size > kMaxRequestLine + kMaxHeaderTotal + kMaxBody) {
            error_ = ParseResult::too_large;
            return error_;
        }
    // 头部已结束，此时的配额只剩请求体，不与头部上限重复计算。
    } else if (buffered_.size() + size > kMaxBody) {
        error_ = ParseResult::too_large;
        return error_;
    }
    buffered_.append(data, size);
    const auto result = progress();
    if (result == ParseResult::complete) {
        // 一条请求结束即丢弃其后的多余字节：本子集不支持 keep-alive 复用与流水线，
        // 保留它们只会干扰下一条请求。下一条请求由下一次 feed() 重新开始解析。
        buffered_.clear();
        finished_ = true;
    }
    return result;
}

// 单步解析：请求行 → 头部 → 请求体，三个阶段各有独立标志，可从任意分段点续跑。
// 返回 complete 时 request_ 完整可读，其后的多余字节由 feed() 统一丢弃。
ParseResult Parser::progress() {
    // 第一阶段：解析请求行。
    // 必须用独立的标志而不是 method.empty() 判断：method 在返回 complete 之后仍然保留给调用方读取，
    // 用它判断会让同一条连接上的下一条请求跳过请求行解析。
    if (!have_request_line_) {
        const auto end = buffered_.find("\r\n");
        if (end == std::string::npos) {
            // 仍然没有行尾：只有在超过请求行上限时才判错。
            if (buffered_.size() > kMaxRequestLine) {
                error_ = ParseResult::too_large;
                return error_;
            }
            return ParseResult::incomplete;
        }
        if (end > kMaxRequestLine) {
            error_ = ParseResult::too_large;
            return error_;
        }
        const auto line = buffered_.substr(0, end);
        const auto first_space = line.find(' ');
        const auto second_space = first_space == std::string::npos ? std::string::npos : line.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos || second_space + 1 >= line.size()) {
            error_ = ParseResult::malformed;
            return error_;
        }
        // 请求行按第一个与第二个空格切成 method / target / version 三段。
        request_.method = line.substr(0, first_space);
        request_.target = line.substr(first_space + 1, second_space - first_space - 1);
        request_.version = line.substr(second_space + 1);
        have_request_line_ = true;
        // 三段都必须非空；方法还必须是 token、目标不得含控制字符，防止请求行注入。
        if (request_.method.empty() || request_.target.empty()) {
            error_ = ParseResult::malformed;
            return error_;
        }
        // 方法必须是 token。
        if (!std::all_of(request_.method.begin(), request_.method.end(),
                [](unsigned char character) { return is_token_character(character); })) {
            error_ = ParseResult::malformed;
            return error_;
        }
        // 目标不得包含控制字符（防止请求行注入）。
        if (std::any_of(request_.target.begin(), request_.target.end(),
                [](unsigned char character) { return character < 0x21 || character == 0x7f; })) {
            error_ = ParseResult::malformed;
            return error_;
        }
        // 只支持 1.0/1.1；其他版本一律拒绝，不做兼容猜测。
        if (request_.version != "HTTP/1.1" && request_.version != "HTTP/1.0") {
            error_ = ParseResult::malformed;
            return error_;
        }
        buffered_.erase(0, end + 2);
    }

    // 第二阶段：解析头部。
    if (!have_headers_) {
        for (;;) {
            const auto end = buffered_.find("\r\n");
            if (end == std::string::npos) {
                if (buffered_.size() > kMaxHeaderLine) {
                    error_ = ParseResult::too_large;
                    return error_;
                }
                return ParseResult::incomplete;
            }
            if (end > kMaxHeaderLine) {
                error_ = ParseResult::too_large;
                return error_;
            }
            // 空行表示头部结束；单个头部行、头部总字节数与头部条数各有独立上限。
            if (end == 0) {
                // 空行：头部结束。
                buffered_.erase(0, 2);
                have_headers_ = true;
                break;
            }
            const auto line = buffered_.substr(0, end);
            header_bytes_ += line.size() + 2;
            if (header_bytes_ > kMaxHeaderTotal) {
                error_ = ParseResult::too_large;
                return error_;
            }
            if (request_.headers.size() >= kMaxHeaders) {
                error_ = ParseResult::too_large;
                return error_;
            }
            // 名字必须是 token 且非空；值去掉前导空白后不得含控制字符（HTAB 例外）。
            const auto colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                error_ = ParseResult::malformed;
                return error_;
            }
            const auto name = line.substr(0, colon);
            if (!std::all_of(name.begin(), name.end(),
                    [](unsigned char character) { return is_token_character(character); })) {
                error_ = ParseResult::malformed;
                return error_;
            }
            // 头部值不得包含控制字符（允许 HTAB）。
            auto value = line.substr(colon + 1);
            const auto first = value.find_first_not_of(" \t");
            value = first == std::string::npos ? std::string{} : value.substr(first);
            if (std::any_of(value.begin(), value.end(), [](unsigned char character) {
                    return (character < 0x20 && character != '\t') || character == 0x7f;
                })) {
                error_ = ParseResult::malformed;
                return error_;
            }
            request_.headers.emplace_back(name, value);
            buffered_.erase(0, end + 2);
        }

        // 头部结束后立刻判定不支持的传输方式与重复 Content-Length。
        // 这些头部语法合法但本实现不支持，判 unsupported 而不是 malformed。
        if (request_.has_header("Transfer-Encoding") || request_.has_header("Expect") || request_.has_header("Upgrade")) {
            error_ = ParseResult::unsupported;
            return error_;
        }
        // 单值头部重复出现即判错：请求走私大多靠重复的 Content-Length 或 Authorization。
        for (const auto* unique : {"Content-Length", "Authorization", "Host", "X-BMC-Generation"}) {
            const auto count = std::count_if(request_.headers.begin(), request_.headers.end(), [unique](const auto& header) {
                return lower(header.first) == lower(unique);
            });
            if (count > 1) { error_ = ParseResult::malformed; return error_; }
        }
        // 只有给出 Content-Length 才收请求体；长度本身也要过 kMaxBody。
        if (request_.has_header("Content-Length")) {
            const auto text = request_.header("Content-Length");
            if (!parse_length(text, body_length_)) {
                error_ = ParseResult::malformed;
                return error_;
            }
            if (body_length_ > kMaxBody) {
                error_ = ParseResult::too_large;
                return error_;
            }
        }
    }

    // 第三阶段：按 Content-Length 收齐请求体。
    // 第三阶段：收满 body_length_ 字节才算完整，POST 的请求体靠 Content-Length 定界。
    if (buffered_.size() < body_length_) {
        return ParseResult::incomplete;
    }
    // 取出请求体；剩余字节留给 feed() 在返回 complete 时清掉（不支持流水线）。
    request_.body = buffered_.substr(0, body_length_);
    buffered_.erase(0, body_length_);
    return ParseResult::complete;
}

// 非法配置（容量、补充速率、键上限）直接抛出，避免静默退化成"全部放行"。
RateLimiter::RateLimiter(double capacity, double refill_per_second, std::size_t max_keys)
    : capacity_(capacity), refill_(refill_per_second), max_keys_(max_keys) {
    if (capacity_ <= 0 || refill_ < 0 || max_keys_ == 0) {
        throw std::invalid_argument("invalid rate limiter configuration");
    }
}

// 令牌桶：按距上次访问的时间补令牌并封顶到容量，不足 1 个即拒绝。
// 新键在表满时先淘汰时间戳最旧的键，否则新管理员会被永久拒绝。
bool RateLimiter::allow(const std::string& key, std::chrono::steady_clock::time_point now) {
    auto found = buckets_.find(key);
    if (found == buckets_.end()) {
        // 满表时回收最久未访问的来源；仍保持内存上限，新管理员不会被永久拒绝。
        if (buckets_.size() >= max_keys_) {
            auto oldest = buckets_.begin();
            for (auto current = std::next(oldest); current != buckets_.end(); ++current)
                if (current->second.stamp < oldest->second.stamp) oldest = current;
            buckets_.erase(oldest);
        }
        found = buckets_.emplace(key, Bucket{capacity_, now}).first;
    }
    auto& bucket = found->second;
    const auto elapsed = std::chrono::duration<double>(now - bucket.stamp).count();
    // 先按经过的秒数补充令牌再封顶：突发额度就是桶容量本身。
    bucket.tokens = std::min(capacity_, bucket.tokens + elapsed * refill_);
    bucket.stamp = now;
    if (bucket.tokens < 1.0) {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

// 只读查询：不更新时间戳也不消耗令牌，仅供诊断与测试使用。
double RateLimiter::tokens(const std::string& key, std::chrono::steady_clock::time_point now) const {
    const auto found = buckets_.find(key);
    if (found == buckets_.end()) {
        return capacity_;
    }
    const auto elapsed = std::chrono::duration<double>(now - found->second.stamp).count();
    return std::min(capacity_, found->second.tokens + elapsed * refill_);
}
}
}
