#include "bmc/core.hpp"
#include <iomanip>
#include <sstream>
#include <stdexcept>

// 状态枚举的文本映射与 JSON 字符串转义，由 include/bmc/core.hpp 声明。
// 状态名直接进入日志、SEL 与事件总线文本，转义保证这些文本嵌入 JSON 后仍然合法。
namespace bmc {
// 枚举到协议字符串的唯一映射：日志、SEL、总线文本都经它转换。
std::string name(State state) {
    switch (state) {
    case State::normal: return "normal";
    case State::warning: return "warning";
    case State::critical: return "critical";
    case State::unavailable: return "unavailable";
    }
    // 非法枚举值直接抛错而不返回空串：日志里出现无法解析的空字段比一次显式失败更糟。
    throw std::invalid_argument("invalid state");
}
// 输出与 src/json.cpp 的字符串写法对齐：只转义 JSON 必需字符，非 ASCII 字节原样保留。
std::string escape(const std::string& value) {
    std::ostringstream output;
    // 必须按 unsigned char 遍历：有符号 char 的高位字节为负，会误满足 < 0x20 而被写成 \u00xx。
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            // 其余控制字符没有短转义形式，统一按 \uXXXX 输出并补足 4 位。
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec;
            } else {
                output << character;
            }
        }
    }
    return output.str();
}
}
