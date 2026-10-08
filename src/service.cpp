#include "bmc/service.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

// 只读 Redfish / Prometheus 端点与 SEL 文本文件的读取解析，声明见 bmc/service.hpp。
// 输出必须与 tools/bmc_manage.py 的 resource()/metrics() 逐字一致，现有 Python 测试才能拿来对拍。
namespace bmc {
namespace {
// tools/bmc_manage.py parses SEL lines with shlex.split. This covers the three default-mode
// forms: single-quoted text, double-quoted text (with \" and \\ escapes), and unquoted text
// with backslash escapes. src/sel.cpp writes with std::quoted, so only the double-quoted
// form occurs in practice.
// 恒返回 true：调用方按字段个数判断行是否合法，这里不需要单独的失败信号。
bool shell_split(const std::string& line, std::vector<std::string>& fields) {
    fields.clear();
    std::size_t index = 0;
    const auto size = line.size();
    while (index < size) {
        while (index < size) {
            const auto character = static_cast<unsigned char>(line[index]);
            const bool space = character == ' ' || character == '\t' || character == '\n' ||
                               character == '\r' || character == '\f' || character == '\v';
            if (!space) {
                break;
            }
            ++index;
        }
        if (index >= size) {
            break;
        }
        std::string field;
        bool open = false;
        while (index < size) {
            const char character = line[index];
            const auto byte = static_cast<unsigned char>(character);
            const bool space = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
                               byte == '\f' || byte == '\v';
            if (!open && space) {
                break;
            }
            if (character == '\'' || character == '"') {
                open = !open;
                ++index;
                continue;
            }
            if (character == '\\' && index + 1 < size) {
                const char next = line[index + 1];
                if (next == '"' || next == '\\') {
                    field += next;
                    index += 2;
                    continue;
                }
            }
            field += character;
            ++index;
        }
        fields.push_back(field);
    }
    return true;
}
// Integer field. Python uses int(fields[0]); require the whole token to be digits so a
// malformed line is skipped instead of being silently truncated.
// 只接受可选的 +/- 与纯十进制数字，且不做上界检查：超长数字会按 int64 溢出，
// 而 Python 的 int() 是任意精度。正常 SEL 里不会出现这种输入。
bool parse_integer(const std::string& text, std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }
    if (index >= text.size()) {
        return false;
    }
    std::int64_t result = 0;
    for (; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        result = result * 10 + (text[index] - '0');
    }
    value = negative ? -result : result;
    return true;
}
// Prometheus label escaping, matching bmc_manage.py's label(): backslash, newline, quote.
// 只处理反斜杠、换行与双引号三种字符，其它字符（包括 \r）原样输出，与 Python 侧完全一致。
std::string label(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (const char character : text) {
        if (character == '\\') {
            output += "\\\\";
        } else if (character == '\n') {
            output += "\\n";
        } else if (character == '"') {
            output += "\\\"";
        } else {
            output += character;
        }
    }
    return output;
}
// Shortest round-trip representation, matching Python's str(float), for Prometheus text.
std::string python_float(double value) { return JsonWriter::number(value); }

// Shared member order for a single entry: the record fields first, then @odata.id last,
// because Python builds it with dict(entry, **{"@odata.id": ...}).
JsonWriter::Value entry_document(const SelEntry& entry, const std::string& odata_id) {
    JsonWriter::Value item = JsonWriter::Value::object()
        .set("Id", JsonWriter::Value::of(std::to_string(entry.id)))
        .set("TimestampMilliseconds", JsonWriter::Value::of(entry.time_ms))
        .set("Sensor", JsonWriter::Value::of(entry.sensor))
        .set("State", JsonWriter::Value::of(entry.state))
        .set("Message", JsonWriter::Value::of(entry.message));
    // Value 键始终存在：缺值写 null 而不是省略该键，这样才能对上 Python 的 dict 输出。
    if (entry.value) {
        item.set("Value", JsonWriter::Value::of(*entry.value));
    } else {
        item.set("Value", JsonWriter::Value::null());
    }
    item.set("@odata.id", JsonWriter::Value::of(odata_id));
    return item;
}
}

// 读取整个 SEL 文件并跳过无法解析的行；文件缺失时返回空列表。
// max_records 为 0 表示不限制，否则只保留最近 max_records 条（等价 Python 的 deque(maxlen=...)）。
std::vector<SelEntry> read_sel_entries(const std::string& path, std::size_t max_records) {
    std::deque<SelEntry> window;
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> fields;
        shell_split(line, fields);
        // 字段数必须是 6：与 Python 侧 len(fields) != 6 的跳过规则一致。
        if (fields.size() != 6) {
            continue;
        }
        SelEntry entry;
        if (!parse_integer(fields[0], entry.id) || !parse_integer(fields[1], entry.time_ms)) {
            continue;
        }
        entry.sensor = fields[2];
        entry.state = fields[3];
        entry.message = fields[4];
        // 对齐 Python 的 float(fields[5])：整段可解析且结果有限才接受，否则整行跳过。
        if (fields[5] != "null") {
            try {
                std::size_t consumed = 0;
                const double parsed = std::stod(fields[5], &consumed);
                if (consumed != fields[5].size() || !std::isfinite(parsed)) {
                    continue;
                }
                entry.value = parsed;
            } catch (const std::exception&) {
                continue;
            }
        }
        // 滚动窗口：满了先弹出最旧的一条，等价于 deque(maxlen=max_records) 的语义。
        if (max_records != 0 && window.size() == max_records) {
            window.pop_front();
        }
        window.push_back(std::move(entry));
    }
    return {window.begin(), window.end()};
}

// 渲染完整的 HTTP/1.1 响应。body 在返回前就已构建完毕，因此 Content-Length 恒存在；
// Cache-Control: no-store 与 Connection: close 也是协议子集里固定的一部分（见 docs/网络设计说明.md）。
std::string HttpResponse::render() const {
    const auto status_text = reason.empty() ? std::to_string(status) : std::to_string(status) + " " + reason;
    std::ostringstream output;
    output << "HTTP/1.1 " << status_text << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
    return output.str();
}

namespace readonly {
// 把路径映射成 Redfish 资源文档；返回 nullopt 表示 404（对应 Python 侧 resource() 返回 None）。
// 分支顺序与每个文档的字段顺序都刻意与 bmc_manage.py 保持一致，改动会直接破坏对拍测试。
std::optional<JsonWriter::Value> resource(const std::string& target, const std::vector<SelEntry>& entries) {
    const std::string base = kSelBase;
    if (target == "/redfish/v1/") {
        return JsonWriter::Value::object()
            .set("@odata.id", JsonWriter::Value::of(target))
            .set("Id", JsonWriter::Value::of(std::string("RootService")))
            .set("Name", JsonWriter::Value::of(std::string("BMC-Lite management")))
            .set("Managers", JsonWriter::Value::object()
                .set("@odata.id", JsonWriter::Value::of(std::string("/redfish/v1/Managers"))));
    }
    if (target == "/redfish/v1/Managers") {
        return JsonWriter::Value::object()
            .set("@odata.id", JsonWriter::Value::of(target))
            .set("Members@odata.count", JsonWriter::Value::of(static_cast<std::int64_t>(1)))
            .set("Members", JsonWriter::Value::array().push(JsonWriter::Value::object()
                .set("@odata.id", JsonWriter::Value::of(std::string("/redfish/v1/Managers/BMC")))));
    }
    if (target == "/redfish/v1/Managers/BMC") {
        return JsonWriter::Value::object()
            .set("@odata.id", JsonWriter::Value::of(target))
            .set("Id", JsonWriter::Value::of(std::string("BMC")))
            .set("Name", JsonWriter::Value::of(std::string("BMC-Lite")))
            .set("LogServices", JsonWriter::Value::object()
                .set("@odata.id", JsonWriter::Value::of(std::string("/redfish/v1/Managers/BMC/LogServices"))));
    }
    if (target == "/redfish/v1/Managers/BMC/LogServices") {
        return JsonWriter::Value::object()
            .set("@odata.id", JsonWriter::Value::of(target))
            .set("Members@odata.count", JsonWriter::Value::of(static_cast<std::int64_t>(1)))
            .set("Members", JsonWriter::Value::array().push(JsonWriter::Value::object()
                .set("@odata.id", JsonWriter::Value::of(base))));
    }
    if (target == base) {
        return JsonWriter::Value::object()
            .set("@odata.id", JsonWriter::Value::of(target))
            .set("Id", JsonWriter::Value::of(std::string("SEL")))
            .set("Name", JsonWriter::Value::of(std::string("System event log")))
            .set("Entries", JsonWriter::Value::object()
                .set("@odata.id", JsonWriter::Value::of(base + "/Entries")));
    }
    if (target == base + "/Entries") {
        auto members = JsonWriter::Value::array();
        // 每个成员都带上自己的 @odata.id，对应 Python 的 dict(entry, **{"@odata.id": ...})。
        for (const auto& entry : entries) {
            members.push(entry_document(entry, base + "/Entries/" + std::to_string(entry.id)));
        }
        return JsonWriter::Value::object()
            .set("@odata.id", JsonWriter::Value::of(target))
            .set("Members@odata.count", JsonWriter::Value::of(static_cast<std::int64_t>(entries.size())))
            .set("Members", std::move(members));
    }
    const std::string prefix = base + "/Entries/";
    if (target.rfind(prefix, 0) == 0) {
        // 前缀已经保证路径里有 '/'，因此取最后一段就是条目标识符。
        const auto identifier = target.substr(target.rfind('/') + 1);
        for (const auto& entry : entries) {
            if (std::to_string(entry.id) == identifier) {
                return entry_document(entry, target);
            }
        }
    }
    return std::nullopt;
}

// 生成 Prometheus 文本。指标名、HELP/TYPE 文案与输出顺序都与 bmc_manage.py 的 metrics() 逐字一致；
// 每个传感器只保留最近一条状态已知的记录，并按传感器名排序输出（std::map 的键序）。
std::string metrics(const std::vector<SelEntry>& entries) {
    std::ostringstream output;
    output << "# HELP bmc_sel_records Number of readable records in the SEL file.\n"
           << "# TYPE bmc_sel_records gauge\n"
           << "bmc_sel_records " << entries.size() << "\n"
           << "# HELP bmc_sel_last_id Highest readable SEL record identifier.\n"
           << "# TYPE bmc_sel_last_id gauge\n";
    std::int64_t last_id = 0;
    // 与 Python 的 max(..., default=0) 一致：没有任何可读记录时该指标为 0。
    for (const auto& entry : entries) {
        last_id = std::max(last_id, entry.id);
    }
    output << "bmc_sel_last_id " << last_id << "\n"
           << "# HELP bmc_sensor_state Last recorded sensor state; exactly one state is 1.\n"
           << "# TYPE bmc_sensor_state gauge\n";

    // Keep only the last record with a known state per sensor, ordered by sensor name.
    static const std::vector<std::string> states = {"normal", "warning", "critical", "unavailable"};
    std::map<std::string, const SelEntry*> latest;
    for (const auto& entry : entries) {
        if (std::find(states.begin(), states.end(), entry.state) != states.end()) {
            latest[entry.sensor] = &entry;
        }
    }
    for (const auto& [sensor, entry] : latest) {
        // 每个传感器固定输出四个状态样本，其中恰好一个为 1，方便 PromQL 直接做比较。
        for (const auto& state : states) {
            output << "bmc_sensor_state{sensor=\"" << label(sensor) << "\",state=\"" << state << "\"} "
                   << (entry->state == state ? 1 : 0) << "\n";
        }
    }
    output << "# HELP bmc_sensor_last_event_value Sensor value at its last recorded state change; not a live sample.\n"
           << "# TYPE bmc_sensor_last_event_value gauge\n";
    for (const auto& [sensor, entry] : latest) {
        // 只在该记录确实带有限数值时输出；缺值或非有限的记录直接略过这个指标。
        if (entry->value && std::isfinite(*entry->value)) {
            output << "bmc_sensor_last_event_value{sensor=\"" << label(sensor) << "\"} "
                   << python_float(*entry->value) << "\n";
        }
    }
    output << "# HELP bmc_sensor_last_event_timestamp_seconds Timestamp of the last recorded state change.\n"
           << "# TYPE bmc_sensor_last_event_timestamp_seconds gauge\n";
    for (const auto& [sensor, entry] : latest) {
        // 毫秒转秒用浮点除法，再按最短往返格式输出，避免整数截断丢掉小数。
        output << "bmc_sensor_last_event_timestamp_seconds{sensor=\"" << label(sensor) << "\"} "
               << python_float(static_cast<double>(entry->time_ms) / 1000.0) << "\n";
    }
    return output.str();
}

// 按路径分派请求：/metrics、/healthz、Redfish 资源，其余一律 404（JSON 错误体）。
HttpResponse handle(const std::string& target, const std::vector<SelEntry>& entries) {
    // Drop the query string, matching urlsplit(self.path).path.
    const auto query = target.find('?');
    const auto path = query == std::string::npos ? target : target.substr(0, query);

    HttpResponse response;
    if (path == "/metrics") {
        response.status = 200;
        response.reason = "OK";
        response.content_type = "text/plain; version=0.0.4; charset=utf-8";
        response.body = metrics(entries);
        return response;
    }
    if (path == "/healthz") {
        // Local liveness probe; deliberately not a Redfish resource.
        response.status = 200;
        response.reason = "OK";
        response.content_type = "text/plain; charset=utf-8";
        response.body = "ok\n";
        return response;
    }
    const auto body = resource(path, entries);
    if (!body) {
        response.status = 404;
        response.reason = "Not Found";
        response.content_type = "application/json; charset=utf-8";
        // 错误体形状与 Python 侧一致，客户端可以按同一套字段处理失败响应。
        response.body = JsonWriter::dump(JsonWriter::Value::object().set("error",
            JsonWriter::Value::object()
                .set("code", JsonWriter::Value::of(std::string("ResourceNotFound")))
                .set("message", JsonWriter::Value::of(std::string("Unknown resource")))));
        return response;
    }
    response.status = 200;
    response.reason = "OK";
    response.content_type = "application/json; charset=utf-8";
    response.body = JsonWriter::dump(*body);
    return response;
}
}
}
