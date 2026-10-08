#include "bmc/core.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace bmc {
Engine::Engine(Config config) : config_(std::move(config)) { validate(config_); }
// 迟滞只影响恢复阈值，去抖则要求候选状态连续出现；两者解决不同类型的抖动。
State Engine::state() const { return state_; }
State Engine::classify(double value) const {
    const double direction = config_.high ? 1.0 : -1.0;
    const double normalized = value * direction;
    const double warning = config_.warning * direction;
    const double critical = config_.critical * direction;
    if (normalized >= critical || (state_ == State::critical && normalized >= critical - config_.hysteresis)) {
        return State::critical;
    }
    if (normalized >= warning || ((state_ == State::warning || state_ == State::critical) && normalized >= warning - config_.hysteresis)) {
        return State::warning;
    }
    return State::normal;
}
std::optional<Event> Engine::update(std::optional<double> sample) {
    State desired;
    if (!sample || !std::isfinite(*sample)) {
        sample.reset();
        pending_count_ = 0;
        failure_count_ = std::min(failure_count_ + 1, config_.failure_limit);
        if (failure_count_ < config_.failure_limit || state_ == State::unavailable) {
            return std::nullopt;
        }
        desired = State::unavailable;
    } else {
        failure_count_ = 0;
        desired = classify(*sample);
        if (desired == state_) {
            pending_count_ = 0;
            return std::nullopt;
        }
        if (pending_count_ == 0 || pending_ != desired) {
            pending_ = desired;
            pending_count_ = 1;
        } else {
            pending_count_ = std::min(pending_count_ + 1, config_.debounce);
        }
        if (pending_count_ < config_.debounce) {
            return std::nullopt;
        }
    }
    Event event {config_.id, state_, desired, sample, sample ? "threshold" : "read_failure", ++sequence_};
    state_ = desired;
    pending_count_ = 0;
    return event;
}
}
