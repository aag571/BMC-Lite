#include "bmc/core.hpp"
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bmc {
RecoveryPolicyEngine::RecoveryPolicyEngine(Executor executor, std::chrono::milliseconds cooldown, unsigned max_attempts, std::size_t concurrency)
    : executor_(std::move(executor)), cooldown_(cooldown), max_attempts_(max_attempts), concurrency_(concurrency) {
    if (!executor_ || cooldown_.count() < 0 || max_attempts_ == 0 || concurrency_ == 0) throw std::invalid_argument("invalid recovery policy");
}
std::optional<RecoveryResult> RecoveryPolicyEngine::submit(const RecoveryRequest& request) {
    std::unique_lock lock(mutex_);
    const auto key = request.sensor + "\n" + request.action;
    const auto now = std::chrono::steady_clock::now();
    if (states_.find(key) == states_.end()) {
        // 已消失传感器的冷却状态到期后可回收；运行中或冷却中的状态绝不能淘汰。
        if (states_.size() >= 4096) {
            for (auto current = states_.begin(); current != states_.end();) {
                if (!current->second.running && now - current->second.last >= cooldown_) current = states_.erase(current);
                else ++current;
            }
        }
        if (states_.size() >= 4096)
            return RecoveryResult{request.rule, request.sensor, request.action, false, false, 0, "state capacity"};
    }
    auto& state = states_[key];
    if (state.running || active_ >= concurrency_) return RecoveryResult{request.rule, request.sensor, request.action, false, false, 0, "concurrency limit"};
    if (state.last.time_since_epoch().count() != 0 && now - state.last < cooldown_) return RecoveryResult{request.rule, request.sensor, request.action, false, false, 0, "cooldown"};
    state.running = true; ++active_;
    lock.unlock();
    // 外部动作不能持有策略锁，否则其他传感器被一个慢设备阻塞。
    // 不跨解锁保存 unordered_map 元素引用，其他提交可能触发 rehash。
    bool success = false; unsigned attempts = 0;
    for (; attempts < max_attempts_ && !success; ++attempts) {
        try { success = executor_(request); } catch (...) { success = false; }
        if (!success && attempts + 1 < max_attempts_) std::this_thread::sleep_for(std::chrono::milliseconds(10 * (1u << std::min(attempts, 6u))));
    }
    lock.lock();
    --active_; states_[key].running = false; states_[key].last = std::chrono::steady_clock::now();
    return RecoveryResult{request.rule, request.sensor, request.action, true, success, attempts, success ? "completed" : "failed"};
}
void RecoveryPolicyEngine::reset(const std::string& sensor) {
    std::lock_guard lock(mutex_);
    for (auto& [key, state] : states_) if (key.rfind(sensor + "\n", 0) == 0) state.last = {};
}
std::size_t RecoveryPolicyEngine::runtime_size() const { std::lock_guard lock(mutex_); return states_.size(); }
}
