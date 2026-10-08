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
    unsigned worker_threads = 2;
    unsigned task_capacity = 64;
    bool enable_actions = false;
    bool check_config = false;
    unsigned http_port = 0;
    std::string http_bind = "127.0.0.1";
    bool http_allow_remote = false;
    unsigned control_port = 0;
    std::string control_bind = "127.0.0.1";
    std::string control_token_file, control_certificate, control_key;
    std::string uplink_address;
    unsigned uplink_port = 0, uplink_capacity = 256;
    std::string peer_address, peer_token_file, peer_ca, peer_server_name, peer_certificate, peer_key;
    std::string peer_bind = "127.0.0.1";
    unsigned peer_port = 0, peer_listen_port = 0, peer_interval_ms = 1000, peer_stale_ms = 5000;
    // --help 出现在任意位置都生效，并在解析出错时优先显示用法。
    bool help = false;
};

// 解析命令行。布尔开关可以出现在任意位置（因此 --config a --enable-actions --ticks 5 也成立），
// 不再要求它们必须放在末尾。未知选项、缺少取值、非法整数都会抛出 std::invalid_argument。
CliOptions parse_options(const std::vector<std::string>& arguments);
// 用法文本（不含程序名），供 --help 与解析失败时输出。
std::string usage();
}
