#include "bmc/cli.hpp"
#include "bmc/core.hpp"
#include "bmc/monitor.hpp"
#include "bmc/action.hpp"
#include "bmc/network.hpp"
#include "bmc/control.hpp"
#include "bmc/uplink.hpp"
#include "bmc/peer.hpp"
#include <map>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <system_error>
#include <unistd.h>
#include <vector>

// 进程入口与生命周期：main() 与 run() 都定义在本文件，没有对应头文件
// （命令行结构体来自 include/bmc/cli.hpp）。本文件是进程唯一负责生命周期的地方：
// 信号掩码与 signalfd、epoll 主循环、SIGHUP 热重载，以及按固定顺序停止线程与刷盘；
// 其余文件只提供可被单测单独驱动的状态机。
namespace {
// 启动期与主循环中 syscall 失败的统一出口：保留 errno 并附上人类可读的动作说明。
[[noreturn]] void fail(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
// 把描述符注册进 epoll；descriptor < 0 说明创建失败，直接抛出而不是带着残缺的事件集继续运行。
void add(int poller, int descriptor, std::uint32_t events) {
    if (descriptor < 0) {
        fail("create event descriptor");
    }
    epoll_event event {};
    event.events = events;
    event.data.fd = descriptor;
    if (::epoll_ctl(poller, EPOLL_CTL_ADD, descriptor, &event) < 0) {
        fail("register event descriptor");
    }
}
// daemon 主体：启动所有子系统、驱动 epoll 主循环、处理 SIGHUP 重载，退出时按依赖顺序
// 停止线程并落盘。返回值即进程退出码，只由日志刷盘、只读 HTTP 是否启动成功以及它是否
// 在运行中失败决定（见文件末尾的 return）；控制面与审计故障一律走异常路径。
int run(const bmc::CliOptions& settings) {
    // OpenSSL 内部写 socket 不使用 MSG_NOSIGNAL；断开的客户端不能杀死整个 daemon。
    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore_pipe.sa_mask);
    if (::sigaction(SIGPIPE, &ignore_pipe, nullptr) < 0) fail("ignore SIGPIPE");
    // 在创建任何线程之前屏蔽这三个信号：此后所有线程都继承该掩码，
    // 信号只会出现在 signalfd 上，不会打断采样线程或工作线程。
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);
    ::sigaddset(&signals, SIGHUP);
    // pthread_sigmask 返回的是错误号本身，不会设置 errno，因此不能用 generic_category 解释它。
    const int signal_result = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (signal_result != 0) {
        throw std::runtime_error(std::string("block signals: ") + std::strerror(signal_result));
    }
    // logger 与 sel 声明在最前，因而最后才析构：异常退出时事件总线仍在排空，
    // 而总线的处理函数按引用持有这两个对象。
    bmc::Logger logger(settings.log);
    bmc::SelStore sel(settings.sel);
    // 声明在事件总线之前，异常退出时总线先排空，再释放上行回调及网络线程。
    std::shared_ptr<bmc::Uplink> uplink;
    if (!settings.uplink_address.empty()) {
        uplink = std::make_shared<bmc::Uplink>(settings.uplink_address, settings.uplink_port, settings.uplink_capacity);
        uplink->start();
    }
    bmc::EventBus bus;
    if (uplink) {
        for (const auto type : {bmc::BusEventType::sensor_state, bmc::BusEventType::configuration,
                               bmc::BusEventType::service, bmc::BusEventType::recovery})
            bus.subscribe(type, [uplink](const bmc::BusEvent& event) { uplink->enqueue(event); });
    }
    // 全部订阅都在发布任何事件之前完成，因此 "started" 等启动事件也会上行。
    bus.subscribe(bmc::BusEventType::sensor_state, [&logger](const bmc::BusEvent& event) {
        logger.action(event.source, "event-bus:" + event.message);
    });
    bus.subscribe(bmc::BusEventType::service, [&sel, &logger](const bmc::BusEvent& event) {
        if (event.source != "peer") return;
        sel.append("peer", "service", event.message, std::nullopt, true);
        logger.action("peer", event.message);
    });
    // 采样线程写入、主循环与退出路径读取：worker 无法写审计日志时立即停机。
    std::atomic_bool worker_failed = false;
    // 进程唯一的系统调用入口：设备读取与动作写入都经它注入，测试可替换为 FakeLinuxIo。
    bmc::PosixLinuxIo io;
    auto sensors = bmc::prepare_sensors(settings.config, io);
    auto configured_rules = std::filesystem::exists(settings.rules) ? bmc::load_rules(settings.rules) : std::vector<bmc::FaultRule>{};
    if (configured_rules.empty()) {
        logger.action("configuration", "no recovery rules configured; threshold monitoring remains active");
        sel.append("configuration", "warning", "no recovery rules configured", std::nullopt, true);
    }
    bmc::FaultRuleEngine rules(std::move(configured_rules));
    // 应用入口选择真实或模拟动作；恢复引擎仅负责去重、重试和冷却。
    std::shared_ptr<bmc::Action> action;
    if (settings.enable_actions) action = std::make_shared<bmc::PwmAction>(io);
    else action = std::make_shared<bmc::LogOnlyAction>();
    bmc::RecoveryPolicyEngine recovery([action](const bmc::RecoveryRequest& request) {
        return action->execute(request);
    }, std::chrono::seconds(30), 3, settings.worker_threads);
    bmc::Monitor monitor;
    bmc::Worker worker(settings.task_capacity, settings.worker_threads);
    // 声明顺序保证异常退出时也先停止控制网络，再销毁回调与基础服务。
    std::unique_ptr<bmc::ControlService> control;
    std::unique_ptr<bmc::ReadOnlyServer> control_network;
    std::uint64_t config_generation = 1;
    // 把"传感器 → 动作路径"快照推给控制面：网络层只能看到这份配置，看不到 MonitorSensor。
    auto configure_control = [&] {
        if (!control) return;
        std::map<std::string, std::string> paths;
        for (const auto& sensor : sensors) paths.emplace(sensor.config.id, sensor.config.action_path);
        control->configure(config_generation, std::move(paths));
    };
    // 控制面是唯一能触发硬件动作的入口：端口与令牌必须成对给出，任何启动失败都直接
    // 让进程起不来，绝不静默降级（systemd Restart=on-failure 依赖非零退出码）。
    if (settings.control_port != 0 || !settings.control_token_file.empty() ||
        !settings.control_certificate.empty() || !settings.control_key.empty()) {
        try {
            if (settings.control_port == 0 || settings.control_token_file.empty())
                throw std::invalid_argument("control requires port and token file together");
            control = std::make_unique<bmc::ControlService>(bmc::load_control_token(settings.control_token_file),
                worker, recovery, [&sel, &logger](const std::string& record) {
                    const auto sync_failures = sel.sync_failures();
                    sel.append("control", "audit", record, std::nullopt, true);
                    if (!sel.flush() || sel.sync_failures() != sync_failures)
                        throw std::runtime_error("control audit persistence failed");
                    logger.action("control", record);
                });
            configure_control();
            // 审计回调把记录写进 SEL 并当场刷盘；持久化失败或同步失败计数增加时抛异常，
            // 网络线程据此停止监听，主循环再以非零码退出。
            control_network = std::make_unique<bmc::ReadOnlyServer>(settings.control_bind, settings.control_port,
                [&control](const bmc::http::Request& request, const std::string& peer) { return control->handle(request, peer); },
                [&control](const std::string& peer, const std::string& reason) { control->reject(peer, reason); },
                settings.control_certificate, settings.control_key);
            std::string error;
            if (!control_network->start(error)) throw std::runtime_error(error);
        } catch (const std::exception& error) {
            if (control_network) control_network->stop();
            logger.action("control", std::string("listener unavailable: ") + error.what());
            throw std::runtime_error(std::string("control unavailable: ") + error.what());
        }
    }
    // 只读 /healthz 与 /metrics 用的采样计数；采样线程递增，网络线程只读。
    std::atomic<std::uint64_t> sample_count{0};
    std::unique_ptr<bmc::PeerHeartbeat> peer;
    std::unique_ptr<bmc::ReadOnlyServer> peer_network;
    if (!settings.peer_address.empty()) {
        try {
            peer = std::make_unique<bmc::PeerHeartbeat>(settings.peer_address, settings.peer_port,
                bmc::load_control_token(settings.peer_token_file), std::chrono::milliseconds(settings.peer_interval_ms),
                std::chrono::milliseconds(settings.peer_stale_ms), [&bus](const std::string& message) {
                    // 心跳线程只投递有界事件；磁盘写入由事件总线线程处理。
                    bus.publish({bmc::BusEventType::service, "peer", message, std::nullopt});
                }, settings.peer_ca, settings.peer_server_name);
            peer_network = std::make_unique<bmc::ReadOnlyServer>(settings.peer_bind, settings.peer_listen_port,
                [&peer](const bmc::http::Request& request, const std::string& source) { return peer->handle(request, source); },
                bmc::ReadOnlyServer::Rejection{}, settings.peer_certificate, settings.peer_key);
            // role 只影响指标标签与错误文本，必须在 start() 之前设置。
            peer_network->role("peer");
            std::string error;
            if (!peer_network->start(error)) throw std::runtime_error(error);
            peer->start();
        } catch (const std::exception& error) {
            if (peer_network) peer_network->stop();
            if (peer) peer->stop();
            logger.action("peer", std::string("listener unavailable: ") + error.what());
            throw std::runtime_error(std::string("peer unavailable: ") + error.what());
        }
    }
    // 只读 HTTP 端点：快照回调只把 SEL 内存态复制出来，磁盘 I/O 不发生在网络线程里。
    bmc::ReadOnlyServer network(settings.http_bind, settings.http_port, [&sel] {
        std::vector<bmc::SelEntry> entries;
        for (const auto& record : sel.query(4096)) entries.push_back({
            static_cast<std::int64_t>(record.id), record.time_ms, record.source, record.state, record.message, record.value});
        return entries;
    }, [&sample_count] { return sample_count.load(); });
    // /metrics 在本角色指标之后追加其余角色的指标；回调在 start() 之前设置，运行中不再改变。
    network.extra_metrics([&] {
        auto result = network.metrics();
        if (control_network) result += control_network->metrics();
        if (control) result += control->metrics();
        if (uplink) result += uplink->metrics();
        if (peer) result += peer->metrics();
        if (peer_network) result += peer_network->metrics();
        return result;
    });
    // 只读 HTTP 启动失败不是致命错误：控制面、上行与 SEL 仍可服务，只体现在最终退出码上。
    std::string network_error;
    const bool network_started = network.start(network_error);
    if (!network_started) {
        logger.action("network", "listener unavailable: " + network_error);
        std::cerr << "bmc-lite: HTTP unavailable: " << network_error << '\n';
    }
    bmc::Fd poller(::epoll_create1(EPOLL_CLOEXEC));
    if (poller.get() < 0) {
        fail("epoll_create1");
    }
    bmc::Fd timer(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    bmc::Fd termination(::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
    add(poller.get(), timer.get(), EPOLLIN);
    add(poller.get(), termination.get(), EPOLLIN);
    itimerspec interval {};
    interval.it_interval.tv_sec = settings.interval_ms / 1000;
    interval.it_interval.tv_nsec = static_cast<long>(settings.interval_ms % 1000) * 1000000;
    interval.it_value = interval.it_interval;
    if (::timerfd_settime(timer.get(), 0, &interval, nullptr) < 0) {
        fail("arm timer");
    }
    bmc::Fd gpio;
    if (!settings.gpio.empty()) {
        gpio = bmc::Fd(::open(settings.gpio.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (gpio.get() < 0) {
            fail("open GPIO");
        }
        std::array<char, 16> initial {};
        if (::pread(gpio.get(), initial.data(), initial.size(), 0) < 0) {
            fail("acknowledge GPIO");
        }
        add(poller.get(), gpio.get(), EPOLLPRI | EPOLLERR);
    }
    logger.action("service", "started");
    bus.publish({bmc::BusEventType::service, "service", "started", std::nullopt});
    logger.flush();
    std::array<epoll_event, 8> events {};
    unsigned completed = 0;
    bool stopping = false;
    // 主循环：每轮先确认网络线程没有失败，再等事件。1 s 超时只是兜底唤醒，
    // 正常情况下由 timerfd 或 signalfd 驱动。
    while (!stopping && !worker_failed.load()) {
        // 控制/心跳监听线程失效时立即退出，让 systemd Restart=on-failure 生效。
        if (control_network && control_network->failed()) throw std::runtime_error("control listener failed");
        if (control && control->failed()) throw std::runtime_error("control completion audit failed");
        if (peer_network && peer_network->failed()) throw std::runtime_error("peer listener failed");
        const int ready = ::epoll_wait(poller.get(), events.data(), static_cast<int>(events.size()), 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("epoll_wait");
        }
        // 一轮最多处理 8 个就绪描述符；stopping 置位后立即放弃剩余事件。
        for (int index = 0; index < ready && !stopping; ++index) {
            const int descriptor = events[static_cast<std::size_t>(index)].data.fd;
            // signalfd：SIGHUP 走原子热重载，SIGINT/SIGTERM 只置停止标志。
            if (descriptor == termination.get()) {
                signalfd_siginfo info {};
                const auto size = ::read(descriptor, &info, sizeof(info));
                if (size != static_cast<ssize_t>(sizeof(info))) {
                    fail("read signal");
                }
                if (info.ssi_signo == SIGHUP) {
                    try {
                        // 传感器与规则各自独立加载；两者都可能抛出，此时不改动任何状态。
                        auto replacement = bmc::prepare_sensors(settings.config, io);
                        auto replacement_rules = std::filesystem::exists(settings.rules)
                            ? bmc::load_rules(settings.rules)
                            : std::vector<bmc::FaultRule>{};
                        // 先把两份新配置都校验完，再改动任何状态，保证重载是原子的：
                        // 失败时旧传感器与旧规则都原样保留。
                        bmc::FaultRuleEngine::validate(replacement_rules);
                        if (replacement_rules.empty()) logger.action("configuration", "no recovery rules configured; threshold monitoring remains active");
                        std::vector<std::string> sensor_ids;
                        sensor_ids.reserve(replacement.size());
                        for (const auto& sensor : replacement) {
                            sensor_ids.push_back(sensor.config.id);
                        }
                        // 校验通过之后、状态变更之前广播：外部看到该代次即可认为重载已生效。
                        const auto message = "validated generation " + std::to_string(config_generation + 1);
                        logger.action("configuration", message);
                        bus.publish({bmc::BusEventType::configuration, "configuration", message, std::nullopt});
                        // 采样集合整体换新：新配置从下一 tick 起生效。
                        sensors.swap(replacement);
                        // 合并而不是整体替换：保留仍然成立的 (规则, 传感器) 状态，
                        // 避免每次 reload 都把已激活规则复位并重复触发一次恢复动作。
                        rules.merge(std::move(replacement_rules), sensor_ids);
                        // 代次递增并同步给控制面与对端，两者据此判断自己看到的是哪一版配置。
                        ++config_generation;
                        configure_control();
                        if (peer) peer->generation(config_generation);
                        logger.flush();
                    } catch (const std::exception& error) {
                        const auto message = std::string("reload rejected; keeping generation ") + std::to_string(config_generation) + ": " + error.what();
                        logger.action("configuration", message);
                        bus.publish({bmc::BusEventType::configuration, "configuration", message, std::nullopt});
                        logger.flush();
                    }
                } else {
                    stopping = true;
                }
            // timerfd：读出的值就是自上次以来错过的 tick 数，采样与规则评估在此推进。
            } else if (descriptor == timer.get()) {
                std::uint64_t expirations = 0;
                const auto size = ::read(descriptor, &expirations, sizeof(expirations));
                if (size != static_cast<ssize_t>(sizeof(expirations))) {
                    fail("read timer");
                }
                monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, worker_failed);
                logger.flush();
                monitor.report_log_degradation(logger, bus, sel);
                ++completed;
                ++sample_count;
                stopping = settings.ticks != 0 && completed >= settings.ticks;
                if (expirations > 1) {
                    logger.action("service", "missed timer ticks: " + std::to_string(expirations - 1));
                }
            // GPIO 中断（EPOLLPRI）：立即补一次采样，不等下一个 tick。
            } else if (descriptor == gpio.get()) {
                std::array<char, 16> value {};
                if (::pread(descriptor, value.data(), value.size(), 0) < 0) {
                    fail("read GPIO event");
                }
                monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, worker_failed);
            }
        }
    }
    // 排空恢复任务，再排空异步事件，最后才能销毁持久化与日志对象。
    // 先停网络再停 Worker：网络线程仍在向 Worker 提交任务，
    // 先停 Worker 会让这些提交直接变成 rejected（已受理的任务由 Worker 自己执行完）。
    network.stop();
    if (peer_network) peer_network->stop();
    if (peer) peer->stop();
    if (control_network) control_network->stop();
    worker.stop();
    bus.publish({bmc::BusEventType::service, "service", "stopping", std::nullopt});
    // 总线排空后所有异步写入都已结束，此后才能安全地统计与刷盘。
    bus.stop();
    if (uplink) {
        uplink->stop();
        const auto state = uplink->stats();
        logger.action("uplink", "sent=" + std::to_string(state.sent) + " dropped=" + std::to_string(state.dropped));
    }
    const auto task_stats = worker.stats();
    if (task_stats.failed) sel.append("scheduler", "failed", "worker failed tasks=" + std::to_string(task_stats.failed), std::nullopt, true);
    logger.action("scheduler", "accepted=" + std::to_string(task_stats.accepted) +
        " completed=" + std::to_string(task_stats.completed) +
        " failed=" + std::to_string(task_stats.failed) +
        " rejected=" + std::to_string(task_stats.rejected));
    // SEL 与日志都使用常驻描述符与批量写入，退出前必须把各自缓冲区落盘。
    const bool sel_flushed = sel.flush();
    logger.action("sel", std::string("flushed=") + (sel_flushed ? "true" : "false") +
        " write_failures=" + std::to_string(sel.write_failures()) +
        " sync_failures=" + std::to_string(sel.sync_failures()) +
        " dropped_bytes=" + std::to_string(sel.dropped_bytes()) +
        " truncated_bytes=" + std::to_string(sel.truncated_bytes()));
    const bool log_flushed = logger.flush();
    logger.action("log", std::string("flushed=") + (log_flushed ? "true" : "false") +
        " write_failures=" + std::to_string(logger.write_failures()) +
        " dropped_bytes=" + std::to_string(logger.dropped_bytes()));
    if (worker_failed.load()) {
        throw std::runtime_error("recovery worker could not write audit log");
    }
    logger.action("service", "stopped");
    if (!log_flushed) std::cerr << "bmc-lite: log flush failed; monitoring ran with degraded logging\n";
    // 退出码只反映日志刷盘、只读监听是否启动成功、以及它是否在运行中失败；
    // SEL 刷盘结果与 worker 故障分别记入日志、抛异常，不在这里被掩盖。
    return log_flushed && network_started && !network.failed() ? 0 : 1;
}
}
// 进程入口：解析命令行后分派，--check-config 只做离线校验，其余进入 run()。
// 解析失败返回 2 并打印用法，运行期异常统一返回 1。
int main(int count, char** arguments) {
    const std::vector<std::string> tokens(arguments + 1, arguments + count);
    // --help 出现在任意位置都生效；解析失败时也优先给出用法而不是只报一行错误。
    bmc::CliOptions settings;
    try {
        settings = bmc::parse_options(tokens);
    } catch (const std::exception& error) {
        std::cerr << "bmc-lite: " << error.what() << '\n' << bmc::usage();
        return 2;
    }
    if (settings.help) {
        std::cout << bmc::usage();
        return 0;
    }
    try {
        // 与 SIGHUP 走同一套校验路径：传感器与规则全部构造成功才算配置有效。
        if (settings.check_config) {
            bmc::PosixLinuxIo io;
            const auto sensors = bmc::prepare_sensors(settings.config, io);
            bmc::FaultRuleEngine validated_rules(std::filesystem::exists(settings.rules) ? bmc::load_rules(settings.rules) : std::vector<bmc::FaultRule>{});
            std::cout << "configuration valid: " << sensors.size() << " sensors\n";
            return 0;
        }
        return run(settings);
    } catch (const std::exception& error) {
        std::cerr << "bmc-lite: " << error.what() << '\n';
        return 1;
    }
}
