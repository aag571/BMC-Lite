#include "bmc/core.hpp"
#include <algorithm>
#include <set>
#include <unordered_set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bmc {
namespace {
// 运行时状态键 = "<规则 id>\n<触发状态>" + "\n" + "<传感器 id>"。
// 把触发状态纳入键，改 trigger 的规则就不会命中旧状态的计数与 active 标志。
std::string runtime_key(const FaultRule& rule) {
    return rule.id + "\n" + std::to_string(static_cast<int>(rule.trigger));
}
// 规则策略校验；构造函数与热重载合并共用同一套判据。
void validate_rules(const std::vector<FaultRule>& rules) {    std::set<std::string> ids;
    for (const auto& rule : rules) {
        if (rule.id.empty() || rule.sensor.empty() || rule.confirmations == 0 || rule.clear_confirmations == 0 ||
            rule.confirmations > 1000000 || rule.clear_confirmations > 1000000 ||
            (rule.action != "increase_fan" && rule.action != "inspect_device") || !ids.insert(rule.id).second)
            throw std::invalid_argument("invalid fault rule policy");
    }
}
}
std::vector<FaultRule> load_rules(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open rules: " + path.string());
    std::vector<FaultRule> rules; std::set<std::string> ids; std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        FaultRule rule; std::string state; std::istringstream fields(line);
        if (!(fields >> rule.id >> rule.sensor >> state >> rule.confirmations >> rule.clear_confirmations >> rule.action))
            throw std::invalid_argument("invalid fault rule");
        if (state == "warning") rule.trigger = State::warning;
        else if (state == "critical") rule.trigger = State::critical;
        else if (state == "unavailable") rule.trigger = State::unavailable;
        else throw std::invalid_argument("invalid fault rule state");
        std::string trailing;
        if ((fields >> trailing) || rule.confirmations == 0 || rule.clear_confirmations == 0 || rule.confirmations > 1000000 || rule.clear_confirmations > 1000000 || rule.id.empty() || rule.sensor.empty() || !ids.insert(rule.id).second)
            throw std::invalid_argument("invalid or duplicate fault rule");
        rules.push_back(std::move(rule));
    }
    return rules;
}
FaultRuleEngine::FaultRuleEngine(std::vector<FaultRule> rules) : rules_(std::move(rules)) {
    validate_rules(rules_);
}
void FaultRuleEngine::reset() { runtime_.clear(); }
void FaultRuleEngine::retain_sensors(const std::vector<std::string>& sensors) {
    const std::unordered_set<std::string> live(sensors.begin(), sensors.end());
    for (auto current = runtime_.begin(); current != runtime_.end();) {
        const auto separator = current->first.rfind('\n');
        if (separator == std::string::npos || live.count(current->first.substr(separator + 1)) == 0)
            current = runtime_.erase(current);
        else ++current;
    }
}

void FaultRuleEngine::validate(const std::vector<FaultRule>& rules) {
    validate_rules(rules);
}

void FaultRuleEngine::merge(std::vector<FaultRule> rules, const std::vector<std::string>& sensors) {
    validate_rules(rules);
    // 第一步：判定哪些 (规则 id, 触发状态) 组合允许继续沿用运行时状态。
    // 触发状态被改写的规则必须复位：留着的 active 对应的是旧的判定条件。
    std::set<std::string> retained;
    for (const auto& rule : rules) {
        retained.insert(runtime_key(rule));
    }
    auto keys = std::vector<std::string>{};
    keys.reserve(runtime_.size());
    for (const auto& entry : runtime_) {
        keys.push_back(entry.first);
    }
    for (const auto& key : keys) {
        // 键格式为 rule_key "\n" sensor；rule_key 为 "<id>\n<trigger>"。
        const auto separator = key.rfind('\n');
        if (separator == std::string::npos) {
            runtime_.erase(key);
            continue;
        }
        const auto rule_key = key.substr(0, separator);
        const auto sensor = key.substr(separator + 1);
        const bool rule_kept = retained.count(rule_key) != 0;
        const bool sensor_kept = std::find(sensors.begin(), sensors.end(), sensor) != sensors.end();
        if (!rule_kept || !sensor_kept) {
            runtime_.erase(key);
        }
    }
    rules_ = std::move(rules);
}
std::vector<RuleDecision> FaultRuleEngine::evaluate(const Event& event) {
    std::vector<RuleDecision> decisions;
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const auto& rule = rules_[index];
        if (rule.sensor != "*" && rule.sensor != event.id) continue;
        // 运行时键必须包含触发状态，否则同一规则改 trigger 后会命中旧状态。
        auto& state = runtime_[runtime_key(rule) + "\n" + event.id];
        const bool bad = event.after == rule.trigger ||
            (rule.trigger == State::warning && event.after == State::critical) ||
            (rule.trigger == State::unavailable && event.after == State::unavailable);
        if (bad) { state.good = 0; state.bad = std::min(state.bad + 1, rule.confirmations); }
        else { state.bad = 0; state.good = std::min(state.good + 1, rule.clear_confirmations); }
        if (!state.active && bad && state.bad >= rule.confirmations) {
            state.active = true; decisions.push_back({rule.id, event.id, event.after, true, rule.action, ++state.sequence});
        } else if (state.active && !bad && state.good >= rule.clear_confirmations) {
            state.active = false; decisions.push_back({rule.id, event.id, event.after, false, "clear", ++state.sequence});
        }
    }
    return decisions;
}
}
