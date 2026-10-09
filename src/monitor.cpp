#include "bmc/monitor.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

// 一轮采样的业务编排：取值 → 状态迁移 → 规则判定 → 异步恢复提交 → 降级上报，
// 声明见 bmc/monitor.hpp。这里不认识 epoll/timerfd/signals，依赖全部由 main.cpp 注入。
namespace bmc {
// 按配置构造传感器（设备 + 状态机）；任一设备构造失败都会让守护进程启动失败。
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
// 跑完一轮采样：写日志/SEL/总线，提交恢复任务，最后汇总降级状况。
// 顺序有意固定：先处理全部样本与规则决策，再做收尾观测，避免观测记录插进决策序列中间。
void Monitor::poll(std::vector<MonitorSensor>& sensors, bmc::EventLogger& logger, bmc::Worker& worker,
            bmc::EventBus& bus, bmc::FaultRuleEngine& rules, bmc::RecoveryPolicyEngine& recovery, bmc::SelStore& sel, std::atomic_bool& worker_failed) {
    for (auto& sensor : sensors) {
        // 每次采样都评估规则，只有状态变化才写入故障历史，避免日志无限重复。
        // 读取失败以 nullopt 表示，由 Engine 累计到 failure_limit 后转成 unavailable。
        const auto value = sensor.read_value();
        const auto transition = sensor.engine.update(value);
        // 没有迁移时也要造一个"当前状态"事件：规则确认需要每个周期的状态快照，
        // 否则连续 critical 不产生新事件，confirmations 大于 1 的规则永远无法触发。
        const bmc::Event event = transition.value_or(bmc::Event{sensor.config.id, sensor.engine.state(), sensor.engine.state(), value, "sample", 0});
        if (transition) {
            logger.write(event);
            // 状态迁移是关键故障证据，立即落盘。
            sel.append(event.id, bmc::name(event.after), event.reason, event.value, true);
            bus.publish({bmc::BusEventType::sensor_state, event.id, bmc::name(event.after), event.value});
        }
        // 规则决策（激活/清除）是恢复动作的依据，同样以关键记录立即落盘。
        for (const auto& decision : rules.evaluate(event)) {
            const auto message = std::string(decision.active ? "activated:" : "cleared:") + decision.action;
            logger.action(decision.rule, message);
            sel.append(decision.rule, decision.active ? "active" : "clear", message, event.value, true);
            bus.publish({bmc::BusEventType::recovery, decision.rule, message, event.value});
            if (decision.active) {
                // 请求复制配置快照；任务只引用生命周期长于线程池的基础服务。
                // 优先级 10 让 Worker 把它插到默认 0 的常规任务之前，故障处置不会被队列拖延
                // （控制面提交手动动作时用的是同一约定）；队列满被拒时只记一条 rejected，不重试。
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
                    // 任务体内的异常置位 worker_failed 并重新抛出，Worker 记为失败任务，主循环据此退出。
                    } catch (...) { worker_failed.store(true); throw; }
                }, 10)) logger.action(decision.rule, "rejected: recovery queue full");
            } else {
                // 规则清除时一并清掉恢复策略的去重/冷却状态，下次激活才能立即再次动作。
                recovery.reset(event.id);
            }
        }
        // 迁移额外回显到 stdout，便于前台运行和 systemd 日志观察。
        if (transition) std::cout << event.id << ": " << bmc::name(event.before) << " -> " << bmc::name(event.after) << '\n';
    }
    // 收尾顺序固定：SEL 降级 → 规则状态裁剪 → 日志降级。
    // 日志降级放最后，因为它自己还会追加一条 SEL 记录和一条总线事件。
    report_sel_degradation(logger, bus, sel);
    std::vector<std::string> live;
    live.reserve(sensors.size());
    for (const auto& sensor : sensors) live.push_back(sensor.config.id);
    // 每轮采样都用当前传感器集合裁剪规则状态，而不是只在热重载时做：
    // 消失传感器的确认计数若留着，会被复用同一 id 的新设备继承，映射也会持续增长。
    rules.retain_sensors(live);
    report_log_degradation(logger, bus, sel);
}

// 上报日志写入降级。只在失败计数或丢弃字节数变化时上报，且两次上报之间至少间隔 5 秒，
// 避免持续失败时每个采样周期都往 SEL 里刷一条记录；没有变化就什么都不做。
void Monitor::report_log_degradation(EventLogger& logger, EventBus& bus, SelStore& sel,
    std::chrono::steady_clock::time_point now) {
    const auto failures = logger.write_failures();
    const auto dropped = logger.dropped_bytes();
    if (failures == reported_log_failures_ && dropped == reported_log_dropped_) return;
    // 5 秒节流只约束"重复上报"；首次上报不受限制，保证降级立刻可见。
    if (log_reported_ && now - last_log_report_ < std::chrono::seconds(5)) return;
    const auto message = "log-write-failures=" + std::to_string(failures) +
        " dropped-bytes=" + std::to_string(dropped);
    // 不向故障 Logger 写回告警，否则告警自身会增加失败计数形成反馈循环。
    sel.append("log", "degraded", message, std::nullopt, true);
    bus.publish({BusEventType::service, "log", message, std::nullopt});
    // 只有真正上报成功后才推进基线：被节流丢掉的那次观测必须留到下一轮补报。
    reported_log_failures_ = failures; reported_log_dropped_ = dropped;
    last_log_report_ = now; log_reported_ = true;
}

// 上报 SEL 写入降级：只在写入失败计数增长时上报一次，并把同步失败、待落盘字节与已丢弃
// 字节一并给出，让"故障证据正在丢失"变成可观测状态。
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
    // 这条上报以非关键方式追加：否则"上报降级"自身就会强制落盘，可能再次失败并推高计数。
    sel.append("sel", "degraded", message);
    bus.publish({bmc::BusEventType::service, "sel", message, std::nullopt});
}

}
