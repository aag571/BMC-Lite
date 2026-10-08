#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bmc {
// 极小的 JSON 写出器，只为与 tools/bmc_manage.py 的 json.dumps 默认输出保持一致：
// 分隔符是 ", " 和 ": "，非 ASCII 原样输出（等价 ensure_ascii=False），
// 且拒绝写出 NaN/Infinity（Python 侧用 allow_nan=False）。
class JsonWriter {
public:
    // 成员值：整数按 Python 的 int 打印，浮点按最短可往返表示打印。
    struct Value {
        enum class Kind { null_value, boolean, integer, real, text, object, array };
        Kind kind = Kind::null_value;
        bool boolean = false;
        std::int64_t integer = 0;
        double real = 0;
        std::string text;
        std::vector<std::pair<std::string, Value>> members;
        std::vector<Value> elements;

        static Value null() { return {}; }
        static Value of(bool value);
        static Value of(std::int64_t value);
        static Value of(double value);
        static Value of(std::string value);
        static Value object();
        static Value array();
        Value& set(std::string name, Value value);
        Value& push(Value value);
    };

    // 生成紧凑但带空格分隔符的 JSON，与 json.dumps(obj) 一致。
    static std::string dump(const Value& value);
    // 浮点格式化：最短可往返表示，且整数值省略小数部分（123.0 -> "123"）。
    // 写出 NaN/Infinity 会抛出 std::invalid_argument。
    static std::string number(double value);
};
}
