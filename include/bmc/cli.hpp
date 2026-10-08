#pragma once
#include <string>
#include <vector>

namespace bmc {
// 命令行选项。默认值与旧实现保持一致，避免行为变化。
struct CliOptions {
    std::string config = "config/mock.conf";
    std::string log = "var/faults.jsonl";
    std::string rules = "config/rules.conf";
    std::string sel = "var/sel.db";
    std::string gpio;
    unsigned interval_ms = 1000;
    unsigned ticks = 0;
    unsigned worker_threads = 1;
    unsigned task_capacity = 64;
    bool enable_actions = false;
    bool check_config = false;
    // --help 出现在任意位置都生效，并在解析出错时优先显示用法。
    bool help = false;
};

// 解析命令行。布尔开关可以出现在任意位置（因此 --config a --enable-actions --ticks 5 也成立），
// 不再要求它们必须放在末尾。未知选项、缺少取值、非法整数都会抛出 std::invalid_argument。
CliOptions parse_options(const std::vector<std::string>& arguments);
// 用法文本（不含程序名），供 --help 与解析失败时输出。
std::string usage();
}
