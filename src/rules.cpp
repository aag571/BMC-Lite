#include "bmc/core.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// 故障规则文件的解析、策略校验与运行时确认状态机，声明见 bmc/core.hpp。
// 这里只负责"从事件到决策"的纯逻辑：不写 SEL、不提交恢复，那些由 monitor.cpp 决定。
namespace bmc {
namespace {
// 运行时状态键 = "<规则 id>\n<触发状态>" + "\n" + "<传感器 id>"。
// 把触发状态纳入键，改 trigger 的规则就不会命中旧状态的计数与 active 标志。
// 本函数只生成键的规则部分，完整的键由调用方再拼上 "\n" + 传感器 id。
std::string runtime_key(const FaultRule& rule) {
    return rule.id + "\n" + std::to_string(static_cast<int>(rule.trigger));
}
// 规则策略校验；构造函数与热重载合并共用同一套判据。
// 约束：id 非空且唯一、传感器非空、确认窗口在 1..1000000、动作名在内置白名单内。
void validate_rules(const std::vector<FaultRule>& rules) {    std::set<std::string> ids;
    for (const auto& rule : rules) {
        if (rule.id.empty() || rule.sensor.empty() || rule.confirmations == 0 || rule.clear_confirmations == 0 ||
            rule.confirmations > 1000000 || rule.clear_confirmations > 1000000 ||
            (rule.action != "increase_fan" && rule.action != "inspect_device") || !ids.insert(rule.id).second)
            throw std::invalid_argument("invalid fault rule policy");
    }
}
}
// 纯文本规则解析；不打开文件，因此领域测试不需要文件系统。
std::vector<FaultRule> parse_rules(std::string_view text) {
    std::vector<FaultRule> rules; std::set<std::string> ids; std::string line;
    std::istringstream input{std::string(text)};
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
        // 多余字段、窗口越界、空 id/传感器与重复规则 id 都由这一个条件兜住，
        // 任一项不满足即整行拒绝；ids 提供跨行的唯一性检查（决策里用 id 标识规则）。
        if ((fields >> trailing) || rule.confirmations == 0 || rule.clear_confirmations == 0 || rule.confirmations > 1000000 || rule.clear_confirmations > 1000000 || rule.id.empty() || rule.sensor.empty() || !ids.insert(rule.id).second)
            throw std::invalid_argument("invalid or duplicate fault rule");
        rules.push_back(std::move(rule));
    }
    return rules;
}
// 构造即整体校验：规则配置错误必须在守护进程启动阶段抛出，而不是等第一条事件到来。
FaultRuleEngine::FaultRuleEngine(std::vector<FaultRule> rules) : rules_(std::move(rules)) {
    validate_rules(rules_);
}
// 清空全部运行时确认状态；完整重置或测试用，热重载必须走 merge 以保留可沿用的计数。
void FaultRuleEngine::reset() { runtime_.clear(); }
// 每次采样都用当前传感器集合裁剪运行时表。设备消失或重载后被移除的传感器，其确认计数与
// active 标志必须一起丢弃：否则运行时表会无界增长，同一 id 被新设备复用时还会错误继承状态。
void FaultRuleEngine::retain_sensors(const std::vector<std::string>& sensors) {
    const std::unordered_set<std::string> live(sensors.begin(), sensors.end());
    for (auto current = runtime_.begin(); current != runtime_.end();) {
        // 键的最后一个 '\n' 之后是传感器 id；规则 id 与触发状态都不含换行。
        const auto separator = current->first.rfind('\n');
        if (separator == std::string::npos || live.count(current->first.substr(separator + 1)) == 0)
            current = runtime_.erase(current);
        else ++current;
    }
}

// 静态入口：热重载先校验新规则集，校验通过才允许触碰运行时状态，从而让重载保持原子。
void FaultRuleEngine::validate(const std::vector<FaultRule>& rules) {
    validate_rules(rules);
}

// 热重载入口：新规则集先整体校验，再决定哪些运行时状态可以沿用。
// 保留的是 bad/good 确认计数、active 标志与决策序号；丢弃的是被删除的规则、消失的传感器，
// 以及触发状态被改写的规则。confirmations/action 的变动不重置状态，因为判定身份没有变。
void FaultRuleEngine::merge(std::vector<FaultRule> rules, const std::vector<std::string>& sensors) {
    validate_rules(rules);
    // 第一步：判定哪些 (规则 id, 触发状态) 组合允许继续沿用运行时状态。
    // 触发状态被改写的规则必须复位：留着的 active 对应的是旧的判定条件。
    std::set<std::string> retained;
    for (const auto& rule : rules) {
        retained.insert(runtime_key(rule));
    }
    // 不能在遍历 runtime_ 的同时删除元素（迭代器会失效），所以先取一份键的快照。
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
        // 只有"规则身份（id + 触发状态）仍存在"且"传感器仍存在"的条目才允许保留。
        const bool rule_kept = retained.count(rule_key) != 0;
        const bool sensor_kept = std::find(sensors.begin(), sensors.end(), sensor) != sensors.end();
        if (!rule_kept || !sensor_kept) {
            runtime_.erase(key);
        }
    }
    // 裁剪完成之后才替换规则集：此前任何异常都不会留下半新半旧的配置。
    rules_ = std::move(rules);
}
// 对一条事件评估所有规则，返回本次真正产生的激活/清除决策。每轮采样都会调用，
// "连续 N 次命中"这类确认语义就依赖这里对所有规则逐条累积计数。
std::vector<RuleDecision> FaultRuleEngine::evaluate(const Event& event) {
    std::vector<RuleDecision> decisions;
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const auto& rule = rules_[index];
        // sensor 支持 "*" 通配，表示该规则作用于任意传感器。
        if (rule.sensor != "*" && rule.sensor != event.id) continue;
        // 运行时键必须包含触发状态，否则同一规则改 trigger 后会命中旧状态。
        auto& state = runtime_[runtime_key(rule) + "\n" + event.id];
        // warning 触发在 critical 时同样算命中（严重度升级），所以升级不会先清除再重新激活；
        // 反过来，critical 触发遇到 warning 不算命中，规则会按 clear_confirmations 正常清除。
        // 第三项与第一项等价，只是把三种触发状态的写法对齐，并非额外语义。
        const bool bad = event.after == rule.trigger ||
            (rule.trigger == State::warning && event.after == State::critical) ||
            (rule.trigger == State::unavailable && event.after == State::unavailable);
        // 计数封顶到各自的确认阈值即可：既满足 >= 比较，又避免长时间命中导致计数无界增长。
        if (bad) { state.good = 0; state.bad = std::min(state.bad + 1, rule.confirmations); }
        else { state.bad = 0; state.good = std::min(state.good + 1, rule.clear_confirmations); }
        // 决策只在跨过确认阈值的那一刻产生一次，避免每个采样周期重复上报；
        // sequence 单调递增，供上层在异步恢复任务里排序。
        if (!state.active && bad && state.bad >= rule.confirmations) {
            state.active = true; decisions.push_back({rule.id, event.id, event.after, true, rule.action, ++state.sequence});
        } else if (state.active && !bad && state.good >= rule.clear_confirmations) {
            state.active = false; decisions.push_back({rule.id, event.id, event.after, false, "clear", ++state.sequence});
        }
    }
    return decisions;
}
}
