#include "bmc/http.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace bmc {
namespace http {
namespace {
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
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    value = result;
    return true;
}
}

std::string Request::header(const std::string& name) const {
    const auto wanted = lower(name);
    for (const auto& [key, value] : headers) {
        if (lower(key) == wanted) {
            return value;
        }
    }
    return {};
}

bool Request::has_header(const std::string& name) const {
    const auto wanted = lower(name);
    return std::any_of(headers.begin(), headers.end(),
        [&wanted](const auto& entry) { return lower(entry.first) == wanted; });
}

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
        request_.method = line.substr(0, first_space);
        request_.target = line.substr(first_space + 1, second_space - first_space - 1);
        request_.version = line.substr(second_space + 1);
        have_request_line_ = true;
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
        if (request_.has_header("Transfer-Encoding")) {
            error_ = ParseResult::unsupported;
            return error_;
        }
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
    if (buffered_.size() < body_length_) {
        return ParseResult::incomplete;
    }
    request_.body = buffered_.substr(0, body_length_);
    buffered_.erase(0, body_length_);
    return ParseResult::complete;
}

RateLimiter::RateLimiter(double capacity, double refill_per_second, std::size_t max_keys)
    : capacity_(capacity), refill_(refill_per_second), max_keys_(max_keys) {
    if (capacity_ <= 0 || refill_ < 0 || max_keys_ == 0) {
        throw std::invalid_argument("invalid rate limiter configuration");
    }
}

bool RateLimiter::allow(const std::string& key, std::chrono::steady_clock::time_point now) {
    auto found = buckets_.find(key);
    if (found == buckets_.end()) {
        // 键数量达到上限时不再新增：宁可对陌生来源保守拒绝，也不让表无界增长。
        if (buckets_.size() >= max_keys_) {
            return false;
        }
        found = buckets_.emplace(key, Bucket{capacity_, now}).first;
    }
    auto& bucket = found->second;
    const auto elapsed = std::chrono::duration<double>(now - bucket.stamp).count();
    bucket.tokens = std::min(capacity_, bucket.tokens + elapsed * refill_);
    bucket.stamp = now;
    if (bucket.tokens < 1.0) {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

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
