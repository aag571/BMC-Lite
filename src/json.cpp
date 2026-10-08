#include "bmc/json.hpp"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace bmc {
namespace {
void escape_into(const std::string& text, std::string& output) {
    // 与 Python json.dumps 的默认行为对齐：转义引号、反斜杠和控制字符，
    // 但不对非 ASCII 做 \u 转义（等价 ensure_ascii=False）。
    output += '"';
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20) {
                char buffer[8] = {};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                output += buffer;
            } else {
                output += character;
            }
            break;
        }
    }
    output += '"';
}
bool parses_back_to(const char* text, double value) {
    double parsed = 0;
    const auto* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, parsed);
    return result.ec == std::errc{} && parsed == value;
}
// 把 C 的指数写法归一成 Python 的写法：至少两位指数、显式正号。
std::string normalize_exponent(std::string text) {
    const auto position = text.find_first_of("eE");
    if (position == std::string::npos) {
        return text;
    }
    std::string mantissa = text.substr(0, position);
    std::string exponent = text.substr(position + 1);
    char sign = '+';
    if (!exponent.empty() && (exponent.front() == '+' || exponent.front() == '-')) {
        sign = exponent.front();
        exponent.erase(0, 1);
    }
    while (exponent.size() > 2 && exponent.front() == '0') {
        exponent.erase(0, 1);
    }
    while (exponent.size() < 2) {
        exponent.insert(exponent.begin(), '0');
    }
    return mantissa + "e" + sign + exponent;
}
// Python 的 float.__repr__ 在指数落在 -4..15 之外时才使用科学计数法，而 C 的 %g
// 从指数 6 起就切换。这里把指数展开成普通十进制，使 1700000000000 不会被写成 1.7e+12。
std::string expand_exponent(const std::string& text) {
    const auto position = text.find_first_of("eE");
    if (position == std::string::npos) {
        return text;
    }
    const std::string mantissa = text.substr(0, position);
    const int exponent = std::stoi(text.substr(position + 1));
    // 只展开安全范围：|value| < 2^53 时整数部分可以精确表示。
    if (exponent < 0 || exponent > 16) {
        return text;
    }
    std::string digits = mantissa;
    const auto dot = digits.find('.');
    int integer_digits = dot == std::string::npos ? static_cast<int>(digits.size()) : static_cast<int>(dot);
    if (dot != std::string::npos) {
        digits.erase(dot, 1);
    }
    const int target_integer_digits = integer_digits + exponent;
    if (target_integer_digits >= static_cast<int>(digits.size())) {
        digits.append(static_cast<std::size_t>(target_integer_digits) - digits.size(), '0');
        return digits;
    }
    digits.insert(static_cast<std::size_t>(target_integer_digits), 1, '.');
    return digits;
}
void dump_into(const JsonWriter::Value& value, std::string& output) {
    switch (value.kind) {
    case JsonWriter::Value::Kind::null_value:
        output += "null";
        return;
    case JsonWriter::Value::Kind::boolean:
        output += value.boolean ? "true" : "false";
        return;
    case JsonWriter::Value::Kind::integer:
        output += std::to_string(value.integer);
        return;
    case JsonWriter::Value::Kind::real:
        output += JsonWriter::number(value.real);
        return;
    case JsonWriter::Value::Kind::text:
        escape_into(value.text, output);
        return;
    case JsonWriter::Value::Kind::object:
        break;
    case JsonWriter::Value::Kind::array:
        output += '[';
        for (std::size_t index = 0; index < value.elements.size(); ++index) {
            if (index != 0) {
                output += ", ";
            }
            dump_into(value.elements[index], output);
        }
        output += ']';
        return;
    }
    output += '{';
    for (std::size_t index = 0; index < value.members.size(); ++index) {
        if (index != 0) {
            output += ", ";
        }
        escape_into(value.members[index].first, output);
        output += ": ";
        dump_into(value.members[index].second, output);
    }
    output += '}';
}
}

JsonWriter::Value JsonWriter::Value::of(bool value) {
    Value result;
    result.kind = Kind::boolean;
    result.boolean = value;
    return result;
}
JsonWriter::Value JsonWriter::Value::of(std::int64_t value) {
    Value result;
    result.kind = Kind::integer;
    result.integer = value;
    return result;
}
JsonWriter::Value JsonWriter::Value::of(double value) {
    Value result;
    result.kind = Kind::real;
    result.real = value;
    return result;
}
JsonWriter::Value JsonWriter::Value::of(std::string value) {
    Value result;
    result.kind = Kind::text;
    result.text = std::move(value);
    return result;
}
JsonWriter::Value JsonWriter::Value::object() {
    Value result;
    result.kind = Kind::object;
    return result;
}
JsonWriter::Value JsonWriter::Value::array() {
    Value result;
    result.kind = Kind::array;
    return result;
}
JsonWriter::Value& JsonWriter::Value::set(std::string name, Value value) {
    kind = Kind::object;
    members.emplace_back(std::move(name), std::move(value));
    return *this;
}
JsonWriter::Value& JsonWriter::Value::push(Value value) {
    kind = Kind::array;
    elements.push_back(std::move(value));
    return *this;
}

std::string JsonWriter::number(double value) {
    if (!std::isfinite(value)) {
        // Python 在 allow_nan=False 时抛 ValueError；这里同样拒绝，而不是写出非法 JSON。
        throw std::invalid_argument("JSON cannot represent NaN or Infinity");
    }
    if (value == 0) {
        return "0";
    }
    // 用精度 17 生成唯一确定该值的十进制串，再逐步降低精度取最短可往返表示，
    // 这与 CPython 的 repr 采用的最短往返策略一致。
    char buffer[64] = {};
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (parses_back_to(buffer, value)) {
            break;
        }
    }
    std::string text = expand_exponent(normalize_exponent(std::string(buffer)));
    // Python prints integral floats without a fractional part (123.0 -> "123"), but only when
    // the fraction is actually zero. Truncating unconditionally would turn 41.5 into 41.
    if (text.find_first_of("eE") == std::string::npos) {
        const auto dot = text.find('.');
        if (dot != std::string::npos && text.find_first_not_of('0', dot + 1) == std::string::npos) {
            text.erase(dot);
        }
    }
    return text;
}

std::string JsonWriter::dump(const Value& value) {
    std::string output;
    dump_into(value, output);
    return output;
}
}
