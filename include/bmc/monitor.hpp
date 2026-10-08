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
    // 调用方刷盘后也可检查；最多每 5 秒报告一次变化，避免持续失败每 tick 刷 SEL。
    void report_log_degradation(EventLogger& logger, EventBus& bus, SelStore& sel,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
private:
    void report_sel_degradation(EventLogger& logger, EventBus& bus, SelStore& sel);
    // 只在 SEL 写入失败次数增加时上报一次，避免每 tick 重复刷屏；
    // 该记录本身以非关键方式追加，因此不会再触发一次落盘尝试。
    std::uint64_t reported_sel_failures_ = 0;
    std::uint64_t reported_log_failures_ = 0, reported_log_dropped_ = 0;
    std::chrono::steady_clock::time_point last_log_report_{};
    bool log_reported_ = false;
};
}
