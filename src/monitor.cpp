#include "bmc/monitor.hpp"
#include <iostream>

namespace bmc {
std::vector<MonitorSensor> prepare_sensors(const std::string& path) {
    std::vector<MonitorSensor> result;
    const auto configs = bmc::load_config(path);
    result.reserve(configs.size());
    for (const auto& config : configs) {
        if (config.backend == "mock") {
            auto validation_reader = bmc::make_reader(config);
        }
        result.emplace_back(config);
    }
    return result;
}
void Monitor::poll(std::vector<MonitorSensor>& sensors, bmc::EventLogger& logger, bmc::Worker& worker,
            bmc::EventBus& bus, bmc::FaultRuleEngine& rules, bmc::RecoveryPolicyEngine& recovery, bmc::SelStore& sel, std::atomic_bool&) {
    for (auto& sensor : sensors) {
        // 每次采样都评估规则，只有状态变化才写入故障历史，避免日志无限重复。
        const auto value = sensor.device->read_value();
        const auto transition = sensor.engine.update(value);
        if (!transition) continue;
        const auto& event = *transition;
        logger.write(event);
        sel.append(event.id, bmc::name(event.after), event.reason, event.value);
        bus.publish({bmc::BusEventType::sensor_state, event.id, bmc::name(event.after), event.value});
        for (const auto& decision : rules.evaluate(event)) {
            const auto message = std::string(decision.active ? "activated:" : "cleared:") + decision.action;
            logger.action(decision.rule, message);
            sel.append(decision.rule, decision.active ? "active" : "clear", message, event.value);
            bus.publish({bmc::BusEventType::recovery, decision.rule, message, event.value});
            if (decision.active) {
                // 请求复制配置快照；任务只引用生命周期长于线程池的基础服务。
                const bmc::RecoveryRequest request{decision.rule, event.id, decision.action, decision.sequence, sensor.config.action_path};
                if (!worker.submit([request, &recovery, &logger, &sel, &bus] {
                        const auto result = recovery.submit(request);
                        if (result) {
                            const auto detail = result->detail + ": attempts=" + std::to_string(result->attempts);
                            logger.action(request.rule, detail);
                            sel.append(request.sensor, "recovery", detail);
                            bus.publish({bmc::BusEventType::recovery, request.sensor, detail, std::nullopt});
                        }
                }, 10)) logger.action(decision.rule, "rejected: recovery queue full");
            } else {
                recovery.reset(event.id);
            }
        }
        std::cout << event.id << ": " << bmc::name(event.before) << " -> " << bmc::name(event.after) << '\n';
    }
}

}
