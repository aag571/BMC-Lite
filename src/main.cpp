#include "bmc/core.hpp"
#include "bmc/monitor.hpp"
#include "bmc/action.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
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

namespace {
struct Options {
    std::string config = "config/mock.conf";
    std::string log = "var/faults.jsonl";
    std::string rules = "config/rules.conf";
    std::string sel = "var/sel.db";
    std::string gpio;
    unsigned interval_ms = 1000;
    unsigned ticks = 0;
    unsigned worker_threads = 1;
    unsigned task_capacity = 64;
    bool enable_actions = false;
    bool check_config = false;
};
unsigned positive(const std::string& token) {
    std::size_t end = 0;
    if (token.empty() || token.front() == '-') {
        throw std::invalid_argument("expected positive integer");
    }
    const auto value = std::stoul(token, &end);
    if (end != token.size() || value == 0 || value > 1000000) {
        throw std::invalid_argument("integer outside supported range");
    }
    return static_cast<unsigned>(value);
}
Options options(int count, char** arguments) {
    Options result;
    for (int index = 1; index < count; ++index) {
        const std::string argument = arguments[index];
        if (argument == "--enable-actions") {
            result.enable_actions = true;
            continue;
        }
        if (argument == "--check-config") {
            result.check_config = true;
            continue;
        }
        if (index + 1 >= count) {
            throw std::invalid_argument("missing option value: " + argument);
        }
        const std::string value = arguments[++index];
        if (argument == "--config") {
            result.config = value;
        } else if (argument == "--rules") {
            result.rules = value;
        } else if (argument == "--sel") {
            result.sel = value;
        } else if (argument == "--log") {
            result.log = value;
        } else if (argument == "--gpio") {
            result.gpio = value;
        } else if (argument == "--interval-ms") {
            result.interval_ms = positive(value);
        } else if (argument == "--ticks") {
            result.ticks = positive(value);
        } else if (argument == "--worker-threads") {
            result.worker_threads = positive(value);
        } else if (argument == "--task-capacity") {
            result.task_capacity = positive(value);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return result;
}
[[noreturn]] void fail(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
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
int run(const Options& settings) {
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);
    ::sigaddset(&signals, SIGHUP);
    const int signal_result = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (signal_result != 0) {
        throw std::system_error(signal_result, std::generic_category(), "block signals");
    }
    bmc::Logger logger(settings.log);
    bmc::SelStore sel(settings.sel);
    bmc::EventBus bus;
    bus.subscribe(bmc::BusEventType::sensor_state, [&logger](const bmc::BusEvent& event) {
        logger.action(event.source, "event-bus:" + event.message);
    });
    std::atomic_bool worker_failed = false;
    // 进程唯一的系统调用入口：设备读取与动作写入都经它注入，测试可替换为 FakeLinuxIo。
    bmc::PosixLinuxIo io;
    auto sensors = bmc::prepare_sensors(settings.config, io);
    bmc::FaultRuleEngine rules(std::filesystem::exists(settings.rules) ? bmc::load_rules(settings.rules) : std::vector<bmc::FaultRule>{});
    // 应用入口选择真实或模拟动作；恢复引擎仅负责去重、重试和冷却。
    std::shared_ptr<bmc::Action> action;
    if (settings.enable_actions) action = std::make_shared<bmc::PwmAction>(io);
    else action = std::make_shared<bmc::LogOnlyAction>();
    bmc::RecoveryPolicyEngine recovery([action](const bmc::RecoveryRequest& request) {
        return action->execute(request);
    }, std::chrono::seconds(30), 3, settings.worker_threads);
    bmc::Monitor monitor;
    bmc::Worker worker(settings.task_capacity, settings.worker_threads);
    std::uint64_t config_generation = 1;
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
    std::array<epoll_event, 8> events {};
    unsigned completed = 0;
    bool stopping = false;
    while (!stopping && !worker_failed.load()) {
        const int ready = ::epoll_wait(poller.get(), events.data(), static_cast<int>(events.size()), 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("epoll_wait");
        }
        for (int index = 0; index < ready && !stopping; ++index) {
            const int descriptor = events[static_cast<std::size_t>(index)].data.fd;
            if (descriptor == termination.get()) {
                signalfd_siginfo info {};
                const auto size = ::read(descriptor, &info, sizeof(info));
                if (size != static_cast<ssize_t>(sizeof(info))) {
                    fail("read signal");
                }
                if (info.ssi_signo == SIGHUP) {
                    try {
                        auto replacement = bmc::prepare_sensors(settings.config, io);
                        bmc::FaultRuleEngine replacement_rules(std::filesystem::exists(settings.rules) ? bmc::load_rules(settings.rules) : std::vector<bmc::FaultRule>{});
                        const auto message = "validated generation " + std::to_string(config_generation + 1);
                        logger.action("configuration", message);
                        bus.publish({bmc::BusEventType::configuration, "configuration", message, std::nullopt});
                        sensors.swap(replacement);
                        rules = std::move(replacement_rules);
                        ++config_generation;
                    } catch (const std::exception& error) {
                        const auto message = std::string("reload rejected; keeping generation ") + std::to_string(config_generation) + ": " + error.what();
                        logger.action("configuration", message);
                        bus.publish({bmc::BusEventType::configuration, "configuration", message, std::nullopt});
                    }
                } else {
                    stopping = true;
                }
            } else if (descriptor == timer.get()) {
                std::uint64_t expirations = 0;
                const auto size = ::read(descriptor, &expirations, sizeof(expirations));
                if (size != static_cast<ssize_t>(sizeof(expirations))) {
                    fail("read timer");
                }
                monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, worker_failed);
                ++completed;
                stopping = settings.ticks != 0 && completed >= settings.ticks;
                if (expirations > 1) {
                    logger.action("service", "missed timer ticks: " + std::to_string(expirations - 1));
                }
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
    worker.stop();
    bus.stop();
    const auto task_stats = worker.stats();
    logger.action("scheduler", "accepted=" + std::to_string(task_stats.accepted) +
        " completed=" + std::to_string(task_stats.completed) +
        " failed=" + std::to_string(task_stats.failed) +
        " rejected=" + std::to_string(task_stats.rejected));
    // SEL 使用常驻描述符与批量写入，退出前必须把缓冲区落盘。
    const bool sel_flushed = sel.flush();
    logger.action("sel", std::string("flushed=") + (sel_flushed ? "true" : "false") +
        " write_failures=" + std::to_string(sel.write_failures()) +
        " sync_failures=" + std::to_string(sel.sync_failures()) +
        " dropped_bytes=" + std::to_string(sel.dropped_bytes()) +
        " truncated_bytes=" + std::to_string(sel.truncated_bytes()));
    if (worker_failed.load()) {
        throw std::runtime_error("recovery worker could not write audit log");
    }
    logger.action("service", "stopped");
    return 0;
}
}
int main(int count, char** arguments) {
    try {
        if (count == 2 && std::string(arguments[1]) == "--help") {
            std::cout << "bmc-lite [--config FILE] [--rules FILE] [--sel FILE] [--log FILE] [--interval-ms N] [--ticks N] [--gpio VALUE_NODE] [--worker-threads N] [--task-capacity N] [--enable-actions] [--check-config]\n";
            return 0;
        }
        const auto settings = options(count, arguments);
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
