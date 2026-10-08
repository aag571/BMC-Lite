#include "bmc/cli.hpp"
#include <charconv>
#include <stdexcept>
#include <string>

namespace bmc {
namespace {
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
std::string usage() {
    return "bmc-lite [--config FILE] [--rules FILE] [--sel FILE] [--log FILE] [--interval-ms N]\n"
           "         [--ticks N] [--gpio VALUE_NODE] [--worker-threads N] [--task-capacity N]\n"
           "         [--enable-actions] [--check-config] [--help]\n";
}

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
        } else if (name.rfind("--", 0) == 0) {
            throw std::invalid_argument("unknown option: " + name);
        } else {
            // 位置参数不被支持：直接报错比静默忽略更安全。
            throw std::invalid_argument("unexpected argument: " + argument);
        }
    }
    return result;
}
}
