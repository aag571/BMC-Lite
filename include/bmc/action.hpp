#pragma once
#include "bmc/core.hpp"

namespace bmc {
// 动作只接受值类型请求，不能保存 Sensor 的地址，因此热加载不影响任务生命周期。
class Action {
public:
    virtual ~Action() = default;
    virtual bool execute(const RecoveryRequest& request) = 0;
};
// 模拟动作不访问硬件，便于虚拟机演示和故障策略单元测试。
class LogOnlyAction final : public Action {
public:
    bool execute(const RecoveryRequest& request) override {
        return request.action == "increase_fan" || request.action == "inspect_device";
    }
};
class PwmAction final : public Action {
public:
    bool execute(const RecoveryRequest& request) override {
        if (request.action == "inspect_device") return true;
        if (request.action != "increase_fan" || request.path.empty()) return false;
        write_pwm(request.path, 255);
        return true;
    }
};
}
