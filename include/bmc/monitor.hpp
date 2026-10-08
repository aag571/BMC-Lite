#pragma once
#include "bmc/core.hpp"
#include <atomic>

namespace bmc {
struct MonitorSensor {
    bmc::Config config;
    std::unique_ptr<bmc::Device> device;
    bmc::Engine engine;
    MonitorSensor(bmc::Config value, LinuxIo& io)
        : config(std::move(value)), device(bmc::make_device(config, io)), engine(config) {}
};

// Monitor 管理一次采样的业务流程；不认识 epoll、timerfd 或 Linux 信号。
// poll 的依赖由应用层注入。Worker 必须在日志、SEL、总线、恢复策略之前停止。
std::vector<MonitorSensor> prepare_sensors(const std::string& path, LinuxIo& io);
class Monitor {
public:
    void poll(std::vector<MonitorSensor>& sensors, EventLogger& logger, Worker& worker,
              EventBus& bus, FaultRuleEngine& rules, RecoveryPolicyEngine& recovery,
              SelStore& sel, std::atomic_bool&);
};
}
