#pragma once
#include "bmc/json.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bmc {
// SEL 文件里可读的一条记录。字段与 src/sel.cpp 的编码一致，也与 tools/bmc_manage.py 的
// records() 解析结果一一对应。
struct SelEntry {
    std::int64_t id = 0;
    std::int64_t time_ms = 0;
    std::string sensor;
    std::string state;
    std::string message;
    std::optional<double> value;
};

// 读取整个 SEL 文件，跳过无法解析的行。文件缺失时返回空列表。
std::vector<SelEntry> read_sel_entries(const std::string& path, std::size_t max_records = 4096);

// HTTP 响应。body 已完全构建，因此 Content-Length 永远是已知的。
struct HttpResponse {
    int status = 200;
    std::string reason;
    std::string content_type;
    std::string body;

    // 按 docs/tcp.md 的协议子集渲染：始终带 Content-Length、Cache-Control: no-store。
    std::string render() const;
};

// 只读端点。刻意与 tools/bmc_manage.py 的 resource()/metrics() 保持逐字一致，
// 从而可以拿现有 Python 测试对拍 C++ 实现。
namespace readonly {
inline constexpr const char* kSelBase = "/redfish/v1/Managers/BMC/LogServices/SEL";

// 返回 nullopt 表示 404（对应 Python 侧 resource() 返回 None）。
std::optional<JsonWriter::Value> resource(const std::string& target, const std::vector<SelEntry>& entries);

// Prometheus 文本，与 bmc_manage.py 的 metrics() 逐字一致。
std::string metrics(const std::vector<SelEntry>& entries);

// 按路径分派：/metrics、/healthz、Redfish 资源，其余 404。
HttpResponse handle(const std::string& target, const std::vector<SelEntry>& entries);
}
}
