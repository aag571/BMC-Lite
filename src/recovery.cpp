#include "bmc/core.hpp"
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

// 恢复策略引擎：按（传感器, 动作）做去重、冷却、并发限制与有限重试，并回收陈旧状态。
// 由 include/bmc/core.hpp 声明；动作通过注入的 executor 执行，因此这里不直接碰硬件。
namespace bmc {
// executor 必须存在，冷却不得为负，尝试次数与并发至少为 1：否则首次提交只会得到无意义的拒绝。
RecoveryPolicyEngine::RecoveryPolicyEngine(Executor executor, std::chrono::milliseconds cooldown, unsigned max_attempts, std::size_t concurrency)
    : executor_(std::move(executor)), cooldown_(cooldown), max_attempts_(max_attempts), concurrency_(concurrency) {
    if (!executor_ || cooldown_.count() < 0 || max_attempts_ == 0 || concurrency_ == 0) throw std::invalid_argument("invalid recovery policy");
}
// 同步执行并返回结果：被接纳的请求一定会跑完重试，拒绝则通过 detail 说明原因。
std::optional<RecoveryResult> RecoveryPolicyEngine::submit(const RecoveryRequest& request) {
    // 用 unique_lock 而不是 lock_guard：外部动作执行期间必须能主动解锁。
    std::unique_lock lock(mutex_);
    // 状态键 = 传感器 + '\n' + 动作：同一传感器的不同动作各有独立冷却，
    // 用换行分隔也避免 (a, bc) 与 (ab, c) 撞到同一个键。
    const auto key = request.sensor + "\n" + request.action;
    const auto now = std::chrono::steady_clock::now();
    // 只有新建键时才做容量检查，已有键的提交不必付出一次全表扫描的代价。
    if (states_.find(key) == states_.end()) {
        // 已消失传感器的冷却状态到期后可回收；运行中或冷却中的状态绝不能淘汰。
        // last 为 epoch 表示该状态从未执行完（本轮刚建），同样落在可回收一侧。
        if (states_.size() >= 4096) {
            for (auto current = states_.begin(); current != states_.end();) {
                if (!current->second.running && now - current->second.last >= cooldown_) current = states_.erase(current);
                else ++current;
            }
        }
        // 回收后仍满则拒绝本次恢复：宁可少做一次动作，也不让状态表无界增长。
        if (states_.size() >= 4096)
            return RecoveryResult{request.rule, request.sensor, request.action, false, false, 0, "state capacity"};
    }
    // 这个引用只在持锁期间有效；一旦解锁，后续一律用 key 重新索引。
    auto& state = states_[key];
    // 两类拒绝分开报：同一（传感器, 动作）已在执行时是去重，不是并发额度不够；
    // 合并成一条文案会让按 detail 分类的调用方误判。running 保证同键串行，
    // concurrency_ 限制全引擎同时在跑的动作数。
    if (state.running) return RecoveryResult{request.rule, request.sensor, request.action, false, false, 0, "already running"};
    if (active_ >= concurrency_) return RecoveryResult{request.rule, request.sensor, request.action, false, false, 0, "concurrency limit"};
    // last 为 epoch 说明该状态还没执行完过（本轮刚建），不受冷却限制。
    // 用 steady_clock 而非系统时钟：冷却窗口不该被时间调整影响。
    if (state.last.time_since_epoch().count() != 0 && now - state.last < cooldown_) return RecoveryResult{request.rule, request.sensor, request.action, false, false, 0, "cooldown"};
    // 占位必须在解锁前完成，否则两个线程会同时通过上面的检查。
    state.running = true; ++active_;
    lock.unlock();
    // 外部动作不能持有策略锁，否则其他传感器被一个慢设备阻塞。
    // 不跨解锁保存 unordered_map 元素引用，其他提交可能触发 rehash。
    bool success = false; unsigned attempts = 0;
    // 这是总预算而非单次上限：每次取剩余预算与指数退避的较小者并扣减，
    // 因此尝试次数再多，累计睡眠也不会超过它。
    auto remaining_backoff = std::chrono::milliseconds(30);
    // 最多 max_attempts_ 次尝试，任一次成功即停；executor 抛出的异常按失败处理，
    // 不让网络或硬件异常逃出恢复流程。
    for (; attempts < max_attempts_ && !success; ++attempts) {
        try { success = executor_(request); } catch (...) { success = false; }
        // 最后一次尝试后不再睡眠：已经没有下一次需要用等待来错开了。
        if (!success && attempts + 1 < max_attempts_) {
            // 即使调用方提高尝试次数，总退避也不超过 30ms，避免长期占用 Worker。
            // 10ms * 2^attempts；指数封顶在 6 只是防止移位溢出，实际取值已被 30ms 预算截断。
            const auto delay = std::min(remaining_backoff,
                std::chrono::milliseconds(10 * (1u << std::min(attempts, 6u))));
            if (delay.count() > 0) std::this_thread::sleep_for(delay);
            remaining_backoff -= delay;
        }
    }
    lock.lock();
    // 重新加锁并按 key 取状态：先前保存的元素引用可能已因 rehash 失效。
    // 成功与失败都写 last：失败后也要等一个冷却窗口，避免对坏设备连续打转。
    --active_; states_[key].running = false; states_[key].last = std::chrono::steady_clock::now();
    // accepted=true 表示请求被接纳并确实执行过，success 才是动作结果，调用方必须区分二者。
    return RecoveryResult{request.rule, request.sensor, request.action, true, success, attempts, success ? "completed" : "failed"};
}
// 把时间戳置回 epoch 即清除冷却，下次提交立即放行；
// 按「传感器 + 换行」前缀匹配，避免误伤 id 以该字符串开头的其他传感器。
void RecoveryPolicyEngine::reset(const std::string& sensor) {
    std::lock_guard lock(mutex_);
    for (auto& [key, state] : states_) if (key.rfind(sensor + "\n", 0) == 0) state.last = {};
}
// 状态表条目数，用于观察回收策略是否生效。
std::size_t RecoveryPolicyEngine::runtime_size() const { std::lock_guard lock(mutex_); return states_.size(); }
}
