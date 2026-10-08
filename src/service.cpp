#include "bmc/service.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace bmc {
namespace {
// tools/bmc_manage.py parses SEL lines with shlex.split. This covers the three default-mode
// forms: single-quoted text, double-quoted text (with \" and \\ escapes), and unquoted text
// with backslash escapes. src/sel.cpp writes with std::quoted, so only the double-quoted
// form occurs in practice.
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
    if (entry.value) {
        item.set("Value", JsonWriter::Value::of(*entry.value));
    } else {
        item.set("Value", JsonWriter::Value::null());
    }
    item.set("@odata.id", JsonWriter::Value::of(odata_id));
    return item;
}
}

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
        if (max_records != 0 && window.size() == max_records) {
            window.pop_front();
        }
        window.push_back(std::move(entry));
    }
    return {window.begin(), window.end()};
}

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
        const auto identifier = target.substr(target.rfind('/') + 1);
        for (const auto& entry : entries) {
            if (std::to_string(entry.id) == identifier) {
                return entry_document(entry, target);
            }
        }
    }
    return std::nullopt;
}

std::string metrics(const std::vector<SelEntry>& entries) {
    std::ostringstream output;
    output << "# HELP bmc_sel_records Number of readable records in the SEL file.\n"
           << "# TYPE bmc_sel_records gauge\n"
           << "bmc_sel_records " << entries.size() << "\n"
           << "# HELP bmc_sel_last_id Highest readable SEL record identifier.\n"
           << "# TYPE bmc_sel_last_id gauge\n";
    std::int64_t last_id = 0;
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
        for (const auto& state : states) {
            output << "bmc_sensor_state{sensor=\"" << label(sensor) << "\",state=\"" << state << "\"} "
                   << (entry->state == state ? 1 : 0) << "\n";
        }
    }
    output << "# HELP bmc_sensor_last_event_value Sensor value at its last recorded state change; not a live sample.\n"
           << "# TYPE bmc_sensor_last_event_value gauge\n";
    for (const auto& [sensor, entry] : latest) {
        if (entry->value && std::isfinite(*entry->value)) {
            output << "bmc_sensor_last_event_value{sensor=\"" << label(sensor) << "\"} "
                   << python_float(*entry->value) << "\n";
        }
    }
    output << "# HELP bmc_sensor_last_event_timestamp_seconds Timestamp of the last recorded state change.\n"
           << "# TYPE bmc_sensor_last_event_timestamp_seconds gauge\n";
    for (const auto& [sensor, entry] : latest) {
        output << "bmc_sensor_last_event_timestamp_seconds{sensor=\"" << label(sensor) << "\"} "
               << python_float(static_cast<double>(entry->time_ms) / 1000.0) << "\n";
    }
    return output.str();
}

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
