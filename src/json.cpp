#include "bmc/json.hpp"
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

// JSON 写出器实现，声明见 bmc/json.hpp。它存在的理由是对齐 Python json.dumps 的默认文本：
// 分隔符 ", " 与 ": "、非 ASCII 原样输出（ensure_ascii=False）、浮点取最短可往返表示。
namespace bmc {
namespace {
// 逐字节处理、不做 UTF-8 解码，因此多字节序列不会被拆开转义。
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
// 判断一段十进制文本能否被精确解析回原值：这是"最短可往返表示"的判定标准。
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
// 从指数 6 起就切换。这里把 |指数| 在 0..15 的写法展开成普通十进制，使 1700000000000
// 不会被写成 1.7e+12；指数 >= 16 时保持科学计数法，与 Python 的 1e+16 一致。
std::string expand_exponent(const std::string& text) {
    const auto position = text.find_first_of("eE");
    if (position == std::string::npos) {
        return text;
    }
    const std::string mantissa = text.substr(0, position);
    const int exponent = std::stoi(text.substr(position + 1));
    // 只展开 0..15：Python 从 1e16 起改用科学计数法，展开到 16 会与参考实现不一致。
    // 该范围内的整数部分可以精确表示（|value| < 2^53）。
    if (exponent < 0 || exponent > 15) {
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
// 递归写出。对象成员之间与数组元素之间都用 ", "，对象键值用 ": "，与 json.dumps 默认分隔符一致；
// 成员顺序即 set()/push() 的插入顺序（Python 的 dict 同样保序）。
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
    // object 不在此处输出：break 出 switch 后走下面的对象分支；其余类型都已提前 return。
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

// 布尔字面量：走 boolean 分支输出 true/false，不参与数值格式化。
JsonWriter::Value JsonWriter::Value::of(bool value) {
    Value result;
    result.kind = Kind::boolean;
    result.boolean = value;
    return result;
}
// 整数按 Python 的 int 原样打印，不经过浮点路径，因此不会出现小数点或科学计数法。
JsonWriter::Value JsonWriter::Value::of(std::int64_t value) {
    Value result;
    result.kind = Kind::integer;
    result.integer = value;
    return result;
}
// 浮点统一交给 JsonWriter::number 做最短往返格式化。
JsonWriter::Value JsonWriter::Value::of(double value) {
    Value result;
    result.kind = Kind::real;
    result.real = value;
    return result;
}
// 文本在写出时才转义；非 ASCII 字节原样保留。
JsonWriter::Value JsonWriter::Value::of(std::string value) {
    Value result;
    result.kind = Kind::text;
    result.text = std::move(value);
    return result;
}
// 空对象；成员顺序即后续 set() 的调用顺序。
JsonWriter::Value JsonWriter::Value::object() {
    Value result;
    result.kind = Kind::object;
    return result;
}
// 空数组；元素顺序即后续 push() 的调用顺序。
JsonWriter::Value JsonWriter::Value::array() {
    Value result;
    result.kind = Kind::array;
    return result;
}
// 追加一个成员并把 kind 置为 object；不做同名去重，重复键会按插入顺序各输出一次。
JsonWriter::Value& JsonWriter::Value::set(std::string name, Value value) {
    kind = Kind::object;
    members.emplace_back(std::move(name), std::move(value));
    return *this;
}
// 追加一个元素并把 kind 置为 array；不做类型校验，调用方负责构造合法结构。
JsonWriter::Value& JsonWriter::Value::push(Value value) {
    kind = Kind::array;
    elements.push_back(std::move(value));
    return *this;
}

// 生成数字文本：先找最短可往返精度，再归一化与展开指数写法。与生成的向量一致，
// 整数值的浮点省略小数部分（95.0 -> "95"）。
std::string JsonWriter::number(double value) {
    if (!std::isfinite(value)) {
        // Python 在 allow_nan=False 时抛 ValueError；这里同样拒绝，而不是写出非法 JSON。
        throw std::invalid_argument("JSON cannot represent NaN or Infinity");
    }
    if (value == 0) {
        // 0（含 -0.0）统一写成 "0"：跳过精度搜索，也避免出现 "-0" 这种非最短文本。
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
    // 整数结果省略小数部分：只在小数点后全为 0 时裁剪，避免把 41.5 截成 41。
    if (text.find_first_of("eE") == std::string::npos) {
        const auto dot = text.find('.');
        if (dot != std::string::npos && text.find_first_not_of('0', dot + 1) == std::string::npos) {
            text.erase(dot);
        }
    }
    return text;
}

// 写出入口：把整棵值树渲染成一个 std::string。
std::string JsonWriter::dump(const Value& value) {
    std::string output;
    dump_into(value, output);
    return output;
}
}
