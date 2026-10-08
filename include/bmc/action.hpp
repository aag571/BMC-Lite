#pragma once
#include "bmc/core.hpp"
#include "bmc/linux_io.hpp"

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
// 真实写入经注入的 LinuxIo，测试可传入 FakeLinuxIo 覆盖 PWM 写入路径。
class PwmAction final : public Action {
public:
    explicit PwmAction(LinuxIo& io) : io_(io) {}
    bool execute(const RecoveryRequest& request) override {
        if (request.action == "inspect_device") return true;
        if (request.action != "increase_fan" || request.path.empty()) return false;
        write_pwm(request.path, 255, io_);
        return true;
    }
private:
    LinuxIo& io_;
};
}
