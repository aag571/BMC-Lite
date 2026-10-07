#include "bmc/core.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <cstring>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <ctime>

namespace bmc {
namespace {
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
    std::set<std::string> ids;
    for (const auto& rule : rules_) {
        if (rule.id.empty() || rule.sensor.empty() || rule.confirmations == 0 || rule.clear_confirmations == 0 ||
            rule.confirmations > 1000000 || rule.clear_confirmations > 1000000 ||
            (rule.action != "increase_fan" && rule.action != "inspect_device") || !ids.insert(rule.id).second)
            throw std::invalid_argument("invalid fault rule policy");
    }
}
void FaultRuleEngine::reset() { runtime_.clear(); }
std::vector<RuleDecision> FaultRuleEngine::evaluate(const Event& event) {
    std::vector<RuleDecision> decisions;
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const auto& rule = rules_[index];
        if (rule.sensor != "*" && rule.sensor != event.id) continue;
        auto& state = runtime_[rule.id + "\n" + event.id];
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
