#pragma once
// 采样循环：传感器准备与每 tick 的读值、状态机推进、分发到日志/SEL/总线/规则/恢复。
// 只做业务流程编排，不接触 epoll、timerfd 或信号。实现见 src/monitor.cpp。
#include "bmc/core.hpp"
#include <atomic>

namespace bmc {
// 一路在管传感器的全部运行时物件：配置、设备与判级状态机，三者生命周期一致。
struct MonitorSensor {
    bmc::Config config;
    std::unique_ptr<bmc::Device> device;
    bmc::Engine engine;
    MonitorSensor(bmc::Config value, LinuxIo& io)
        : config(std::move(value)), device(bmc::make_device(config, io)), engine(config) {}
};

// Monitor 管理一次采样的业务流程；不认识 epoll、timerfd 或 Linux 信号。
// poll 的依赖由应用层注入。Worker 必须在日志、SEL、总线、恢复策略之前停止。
// 加载配置并构造全部传感器；配置非法时抛出 std::invalid_argument（来自 load_config 与 Engine）。
std::vector<MonitorSensor> prepare_sensors(const std::string& path, LinuxIo& io);
// 一次采样的流程编排者：不持有传感器与设备，因此可在同一进程内复用；
// 唯一自身状态是降级上报的节流游标。
class Monitor {
public:
    // 执行一轮采样：读值、推进状态机、写日志与 SEL、发总线事件、评估规则并提交恢复动作。
    // 需要停止时由调用方通过最后一个原子标志请求，本方法不阻塞等待。
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
