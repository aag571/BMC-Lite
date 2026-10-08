#include "bmc/core.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

// 单传感器状态机：迟滞回收、去抖确认与读失败降级都在这里判定。
// 由 include/bmc/core.hpp 的 Engine 声明；classify 只做判定，update 才推进状态。
namespace bmc {
// 构造即校验策略：非法阈值/窗口在启动时就抛错，而不是留到第一次采样才暴露。
Engine::Engine(Config config) : config_(std::move(config)) { validate(config_); }
// 迟滞只影响恢复阈值，去抖则要求候选状态连续出现；两者解决不同类型的抖动。
// 只返回已确认状态：去抖期间的候选状态对外不可见，调用方看到的一定是稳定状态。
State Engine::state() const { return state_; }
// 纯判定：const 且不修改任何成员，读失败、去抖与序号推进全部由 update() 负责。
// 方向归一后 high/low 共用同一套 >= 比较；归一后 warning 仍在 normal 一侧，
// 所以必须先判 critical 再判 warning，两个分支的先后不可交换。
State Engine::classify(double value) const {
    // high 越大越坏、low 越小越坏：乘 direction 归一后，越大越坏对所有传感器统一成立。
    const double direction = config_.high ? 1.0 : -1.0;
    const double normalized = value * direction;
    const double warning = config_.warning * direction;
    const double critical = config_.critical * direction;
    // 迟滞只放宽已经处于该状态时的回落门槛；从低状态升级仍必须严格越过 critical。
    if (normalized >= critical || (state_ == State::critical && normalized >= critical - config_.hysteresis)) {
        return State::critical;
    }
    // 从 warning 或 critical 回落时同样用放宽后的 warning 门槛，避免在阈值附近来回跳变。
    if (normalized >= warning || ((state_ == State::warning || state_ == State::critical) && normalized >= warning - config_.hysteresis)) {
        return State::warning;
    }
    return State::normal;
}
// 状态推进的唯一入口：采样 -> 判定候选 -> 去抖确认 -> 需要时产生一次迁移事件。
std::optional<Event> Engine::update(std::optional<double> sample) {
    State desired;
    // 非有限值（NaN/Inf）不能当作测量值：NaN 与任何阈值比较都为假，会静默判成 normal，
    // 因此一律按读取失败处理，走与真实读失败完全相同的降级路径。
    if (!sample || !std::isfinite(*sample)) {
        sample.reset();
        // 候选状态已不可信，去抖计数清零：读取失败与阈值判定是两条互不相关的路径。
        pending_count_ = 0;
        // 计数封顶在 failure_limit：它只用来判断是否够降级门槛，继续累加没有意义，
        // 也能避免长期故障下计数器溢出。
        failure_count_ = std::min(failure_count_ + 1, config_.failure_limit);
        // 未达门槛、或已处于 unavailable，都不产生事件：降级事件只在跨过门槛那一刻报一次。
        if (failure_count_ < config_.failure_limit || state_ == State::unavailable) {
            return std::nullopt;
        }
        desired = State::unavailable;
    } else {
        // 一次成功读取即清零：failure_count_ 是连续失败次数，不是累计次数。
        failure_count_ = 0;
        desired = classify(*sample);
        // 候选与已确认状态一致：稳定态，丢掉去抖计数后直接返回。
        if (desired == state_) {
            pending_count_ = 0;
            return std::nullopt;
        }
        // 候选状态一变就从 1 重新计数：否则在阈值附近来回跳动的采样会被累加成一次确认。
        if (pending_count_ == 0 || pending_ != desired) {
            pending_ = desired;
            pending_count_ = 1;
        } else {
            // 封顶在 debounce：达到即可确认，长稳期间没必要继续增长。
            pending_count_ = std::min(pending_count_ + 1, config_.debounce);
        }
        // 要求同一候选连续出现 debounce 次，单点毛刺不改变对外状态。
        if (pending_count_ < config_.debounce) {
            return std::nullopt;
        }
    }
    // 失败分支已把 sample 置空，因此 reason 与 value 由同一条件一致地决定，
    // 不会出现「报了值却说是读失败」的记录。
    // 序号只在这里递增：它标识状态迁移的顺序，供日志与总线做顺序校验。
    Event event {config_.id, state_, desired, sample, sample ? "threshold" : "read_failure", ++sequence_};
    // 提交迁移后清空去抖计数，下一次迁移从零开始重新累计。
    state_ = desired;
    pending_count_ = 0;
    return event;
}
}
