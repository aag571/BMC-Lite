#include "bmc/monitor.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace bmc {
std::vector<MonitorSensor> prepare_sensors(const std::string& path, LinuxIo& io) {
    std::vector<MonitorSensor> result;
    const auto configs = bmc::load_config(path);
    result.reserve(configs.size());
    for (const auto& config : configs) {
        // mock 序列的合法性由 make_device 内部构造 reader 时校验，这里不再预构造一个被丢弃的读取器。
        result.emplace_back(config, io);
    }
    return result;
}
void Monitor::poll(std::vector<MonitorSensor>& sensors, bmc::EventLogger& logger, bmc::Worker& worker,
            bmc::EventBus& bus, bmc::FaultRuleEngine& rules, bmc::RecoveryPolicyEngine& recovery, bmc::SelStore& sel, std::atomic_bool& worker_failed) {
    for (auto& sensor : sensors) {
        // 每次采样都评估规则，只有状态变化才写入故障历史，避免日志无限重复。
        const auto value = sensor.device->read_value();
        const auto transition = sensor.engine.update(value);
        const bmc::Event event = transition.value_or(bmc::Event{sensor.config.id, sensor.engine.state(), sensor.engine.state(), value, "sample", 0});
        if (transition) {
            logger.write(event);
            // 状态迁移是关键故障证据，立即落盘。
            sel.append(event.id, bmc::name(event.after), event.reason, event.value, true);
            bus.publish({bmc::BusEventType::sensor_state, event.id, bmc::name(event.after), event.value});
        }
        for (const auto& decision : rules.evaluate(event)) {
            const auto message = std::string(decision.active ? "activated:" : "cleared:") + decision.action;
            logger.action(decision.rule, message);
            sel.append(decision.rule, decision.active ? "active" : "clear", message, event.value, true);
            bus.publish({bmc::BusEventType::recovery, decision.rule, message, event.value});
            if (decision.active) {
                // 请求复制配置快照；任务只引用生命周期长于线程池的基础服务。
                const bmc::RecoveryRequest request{decision.rule, event.id, decision.action, decision.sequence, sensor.config.action_path};
                if (!worker.submit([request, &recovery, &logger, &sel, &bus, &worker_failed] {
                    try {
                        const auto result = recovery.submit(request);
                        if (result) {
                            const auto detail = result->detail + ": attempts=" + std::to_string(result->attempts);
                            logger.action(request.rule, detail);
                            sel.append(request.sensor, "recovery", detail, std::nullopt, true);
                            bus.publish({bmc::BusEventType::recovery, request.sensor, detail, std::nullopt});
                        }
                    } catch (...) { worker_failed.store(true); throw; }
                }, 10)) logger.action(decision.rule, "rejected: recovery queue full");
            } else {
                recovery.reset(event.id);
            }
        }
        if (transition) std::cout << event.id << ": " << bmc::name(event.before) << " -> " << bmc::name(event.after) << '\n';
    }
    report_sel_degradation(logger, bus, sel);
}

void Monitor::report_sel_degradation(EventLogger& logger, EventBus& bus, SelStore& sel) {
    // 写入失败不再终止服务：这里把失败次数与丢弃字节数上报一次，把"故障证据正在丢失"这件事
    // 变成可观测状态，而不是让整个 daemon 消失。上报记录本身不设为关键，避免每次失败都重试落盘。
    const auto failures = sel.write_failures();
    if (failures == reported_sel_failures_) {
        return;
    }
    reported_sel_failures_ = failures;
    const auto message = "sel-write-failures=" + std::to_string(failures) +
        " sync-failures=" + std::to_string(sel.sync_failures()) +
        " pending=" + std::to_string(sel.pending_bytes()) +
        " dropped=" + std::to_string(sel.dropped_bytes());
    logger.action("sel", message);
    sel.append("sel", "degraded", message);
    bus.publish({bmc::BusEventType::service, "sel", message, std::nullopt});
}

}
