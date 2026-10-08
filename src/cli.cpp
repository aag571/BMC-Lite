#include "bmc/cli.hpp"
#include <charconv>
#include <stdexcept>
#include <string>

// cli.cpp —— 命令行解析（声明见 bmc/cli.hpp）。解析是纯函数：不访问文件系统与环境变量，
// 非法输入一律抛 std::invalid_argument；跨选项一致性在全部参数扫完之后统一校验，与书写顺序无关。
namespace bmc {
namespace {
// 正整数取值：显式拒绝空串与前导 '-'（from_chars 不接受符号，也不接受 0x 前缀），范围固定 1..1000000；
// 端口、容量、间隔等所有数值选项都经这里，各自再按需收紧上限。
unsigned positive(const std::string& token, const std::string& option) {
    if (token.empty() || token.front() == '-') {
        throw std::invalid_argument(option + " expects a positive integer, got: " + token);
    }
    unsigned value = 0;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value == 0 || value > 1000000) {
        throw std::invalid_argument(option + " expects an integer in 1..1000000, got: " + token);
    }
    return value;
}
// --name=value 形式。
// 只在第一个 '=' 处切分，值里可以再含 '='（如路径）；名字为空（"=value"）会落到未知选项分支被拒。
bool split_assignment(const std::string& argument, std::string& name, std::string& value) {
    const auto separator = argument.find('=');
    if (separator == std::string::npos) {
        return false;
    }
    name = argument.substr(0, separator);
    value = argument.substr(separator + 1);
    return true;
}
}
// 用法文本（不含程序名）：--help 与解析失败两条路径共用，改选项时必须同步这里，否则用法与实现漂移。
std::string usage() {
    return "bmc-lite [--config FILE] [--rules FILE] [--sel FILE] [--log FILE] [--interval-ms N]\n"
           "         [--ticks N] [--gpio VALUE_NODE] [--worker-threads N] [--task-capacity N]\n"
           "         [--enable-actions] [--check-config] [--http-port N] [--http-bind ADDR] [--help]\n"
           "         [--http-allow-remote]\n"
           "         [--control-port N --control-token-file FILE] [--control-bind ADDR]\n"
           "         [--control-tls-cert FILE --control-tls-key FILE]\n"
           "         [--uplink-address IPv4 --uplink-port N] [--uplink-capacity N]\n"
           "         [--peer-address IPv4 --peer-port N --peer-listen-port N --peer-token-file FILE]\n"
           "         [--peer-bind IPv4] [--peer-interval-ms N] [--peer-stale-ms N]\n"
           "         [--peer-ca FILE --peer-server-name NAME --peer-tls-cert FILE --peer-tls-key FILE]\n"
           "HTTP is disabled by default; --http-port enables the read-only listener.\n";
}

// 两种写法等价：`--name value`（向后取一个参数）与 `--name=value`；布尔开关不带值。
// 取值的选项后面没有参数时报 missing value；未知的 --xxx 与裸位置参数都直接报错。
// help 为 true 时立刻返回默认结果，其余选项一律不再校验（--help 优先于任何非法参数）。
CliOptions parse_options(const std::vector<std::string>& arguments) {
    CliOptions result;
    // 先扫一遍 --help：它出现即生效，并且优先于任何非法参数。
    for (const auto& argument : arguments) {
        if (argument == "--help" || argument == "-h") {
            result.help = true;
            return result;
        }
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        // 布尔开关，不需要取值，可以出现在任意位置。
        if (argument == "--enable-actions") {
            result.enable_actions = true;
            continue;
        }
        if (argument == "--check-config") {
            result.check_config = true;
            continue;
        }
        if (argument == "--http-allow-remote") {
            result.http_allow_remote = true;
            continue;
        }
        std::string name = argument;
        std::string value;
        const bool assigned = split_assignment(argument, name, value);
        if (!assigned) {
            // 需要取值的选项：向后取一个参数。
            if (index + 1 >= arguments.size()) {
                throw std::invalid_argument("missing value for option: " + argument);
            }
            value = arguments[++index];
        }
        // 选项分发；端口类选项在 1..1000000 之上再收紧到 1..65535，--uplink-capacity 上限 4096。
        if (name == "--config") {
            result.config = value;
        } else if (name == "--rules") {
            result.rules = value;
        } else if (name == "--sel") {
            result.sel = value;
        } else if (name == "--log") {
            result.log = value;
        } else if (name == "--gpio") {
            result.gpio = value;
        } else if (name == "--interval-ms") {
            result.interval_ms = positive(value, name);
        } else if (name == "--ticks") {
            result.ticks = positive(value, name);
        } else if (name == "--worker-threads") {
            result.worker_threads = positive(value, name);
        } else if (name == "--task-capacity") {
            result.task_capacity = positive(value, name);
        } else if (name == "--http-port") {
            result.http_port = positive(value, name);
            if (result.http_port > 65535) throw std::invalid_argument("--http-port must be in 1..65535");
        } else if (name == "--http-bind") {
            result.http_bind = value;
        } else if (name == "--control-port") {
            result.control_port = positive(value, name);
            if (result.control_port > 65535) throw std::invalid_argument("--control-port must be in 1..65535");
        } else if (name == "--control-bind") {
            result.control_bind = value;
        } else if (name == "--control-token-file") {
            result.control_token_file = value;
        } else if (name == "--control-tls-cert") {
            result.control_certificate = value;
        } else if (name == "--control-tls-key") {
            result.control_key = value;
        } else if (name == "--uplink-address") {
            result.uplink_address = value;
        } else if (name == "--uplink-port") {
            result.uplink_port = positive(value, name);
            if (result.uplink_port > 65535) throw std::invalid_argument("--uplink-port must be in 1..65535");
        } else if (name == "--uplink-capacity") {
            result.uplink_capacity = positive(value, name);
            if (result.uplink_capacity > 4096) throw std::invalid_argument("--uplink-capacity must be in 1..4096");
        } else if (name == "--peer-address") { result.peer_address = value;
        } else if (name == "--peer-bind") { result.peer_bind = value;
        } else if (name == "--peer-token-file") { result.peer_token_file = value;
        } else if (name == "--peer-ca") { result.peer_ca = value;
        } else if (name == "--peer-server-name") { result.peer_server_name = value;
        } else if (name == "--peer-tls-cert") { result.peer_certificate = value;
        } else if (name == "--peer-tls-key") { result.peer_key = value;
        } else if (name == "--peer-port" || name == "--peer-listen-port") {
            const auto port = positive(value, name);
            if (port > 65535) throw std::invalid_argument(name + " must be in 1..65535");
            if (name == "--peer-port") result.peer_port = port; else result.peer_listen_port = port;
        } else if (name == "--peer-interval-ms") { result.peer_interval_ms = positive(value, name);
        } else if (name == "--peer-stale-ms") { result.peer_stale_ms = positive(value, name);
        } else if (name.rfind("--", 0) == 0) {
            throw std::invalid_argument("unknown option: " + name);
        } else {
            // 位置参数不被支持：直接报错比静默忽略更安全。
            throw std::invalid_argument("unexpected argument: " + argument);
        }
    }
    // 跨选项一致性检查：全部参数扫完后执行，因此与选项的书写顺序无关。
    // 回环判定只认 "127." 前缀，localhost 等名字不在此特判，非 127.* 的绑定地址必须显式授权。
    if (result.http_port != 0 && result.http_bind.rfind("127.", 0) != 0 && !result.http_allow_remote)
        throw std::invalid_argument("non-loopback HTTP requires --http-allow-remote");
    // 上行只给地址不给端口（或反过来）都算漏配，两者必须同时出现。
    if (result.uplink_address.empty() != (result.uplink_port == 0))
        throw std::invalid_argument("uplink requires address and port together");
    // 只要出现过任一 peer 选项（含 CA/证书/SNI），就必须配齐地址、端口、监听端口与 token 文件。
    const bool peer = !result.peer_address.empty() || result.peer_port || result.peer_listen_port ||
        !result.peer_token_file.empty() || !result.peer_ca.empty() || !result.peer_server_name.empty() ||
        !result.peer_certificate.empty() || !result.peer_key.empty();
    if (peer && (result.peer_address.empty() || !result.peer_port || !result.peer_listen_port || result.peer_token_file.empty()))
        throw std::invalid_argument("peer requires address, port, listen-port and token-file together");
    // 时间参数另有约束：间隔下限 10 ms，且 stale 超时必须严格大于间隔（相等也拒绝）。
    if (peer && (result.peer_interval_ms < 10 || result.peer_stale_ms <= result.peer_interval_ms))
        throw std::invalid_argument("peer interval must be >=10 ms and stale timeout must exceed interval");
    return result;
}
}
