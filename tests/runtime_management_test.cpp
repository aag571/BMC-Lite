#include "bmc/monitor.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

// 运行期自监控：日志与 SEL 的降级如何被计数并上报（含 5 秒节流，避免每 tick 刷屏），
// 以及规则引擎在传感器消失时如何裁剪状态、又如何保留仍在管传感器的确认计数。
namespace {
// EventLogger 的替身：不落任何字节，只给出可控的失败/丢弃计数，用来驱动降级上报分支。
//   - failures/dropped 由用例直接设值，等价于 Logger 内部计数器的读数。
//   - write()/action() 只累加 writes，便于断言降级路径没有反向写日志形成回馈。
class HealthLogger final : public bmc::EventLogger {
public:
    std::uint64_t failures = 0, dropped = 0;
    unsigned writes = 0;
    std::vector<bmc::Event> events;
    void write(const bmc::Event& event) override { ++writes; events.push_back(event); }
    void action(const std::string&, const std::string&) override { ++writes; }
    std::uint64_t write_failures() const override { return failures; }
    std::uint64_t dropped_bytes() const override { return dropped; }
};
// 每个用例独占的文件名，避免并行执行时互相踩踏。
std::filesystem::path storage(const std::string& suffix) {
    return std::filesystem::temp_directory_path() / ("bmc-runtime-management-" + std::to_string(::getpid()) + suffix);
}
TEST(RuntimeManagement, ConfiguredCalibrationReachesThresholdAndSel) {
    const auto config_path = storage("-calibration.conf");
    const auto sel_path = storage("-calibration.sel");
    std::filesystem::remove(sel_path);
    {
        std::ofstream config(config_path);
        config << "cpu mock 45,err 1 high 70 85 3 1 1 - 2;0=0;100=100\n";
    }
    auto sensors = bmc::prepare_sensors(config_path.string(), bmc::system_io());
    bmc::SelStore sel(sel_path);
    bmc::EventBus bus;
    bmc::Worker worker(4, 1);
    bmc::FaultRuleEngine rules({});
    bmc::RecoveryPolicyEngine recovery([](const bmc::RecoveryRequest&) { return true; });
    bmc::Monitor monitor;
    HealthLogger logger;
    std::atomic_bool failed{false};
    monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, failed);
    monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, failed);
    worker.stop(); bus.stop();
    ASSERT_EQ(logger.events.size(), 2u);
    ASSERT_TRUE(logger.events[0].value);
    EXPECT_DOUBLE_EQ(*logger.events[0].value, 90);
    EXPECT_EQ(logger.events[0].after, bmc::State::critical);
    EXPECT_FALSE(logger.events[1].value);
    EXPECT_EQ(logger.events[1].after, bmc::State::unavailable);
    const auto history = sel.query();
    ASSERT_GE(history.size(), 2u);
    ASSERT_TRUE(history[0].value);
    EXPECT_DOUBLE_EQ(*history[0].value, 90);
    std::filesystem::remove(config_path);
    std::filesystem::remove(sel_path);
}
TEST(RuntimeManagement, CalibrationOverflowBecomesReadFailure) {
    bmc::Config config;
    config.id = "cpu";
    config.backend = "mock";
    config.path = "1e308";
    config.calibration.gain = 10;
    bmc::MonitorSensor sensor(config, bmc::system_io());
    EXPECT_FALSE(sensor.read_value());
}
}
// 同一失败在 5 秒窗口内只上报一次；窗口外仍有新失败才再记一条，且上报本身不反写日志。
TEST(RuntimeManagement, LoggerFailureReportsToSelWithoutFeedbackOrSpam) {
    const auto path = storage("-log.sel");
    std::filesystem::remove(path);
    {
        bmc::Logger logger("/dev/full");
        bmc::SelStore sel(path);
        bmc::EventBus bus;
        bmc::Monitor monitor;
        logger.action("test", "failure");
        const auto before = logger.write_failures();
        ASSERT_GT(before, 0u);
        const auto now = std::chrono::steady_clock::time_point{};
        monitor.report_log_degradation(logger, bus, sel, now);
        for (unsigned index = 0; index < 100; ++index)
            monitor.report_log_degradation(logger, bus, sel, now + std::chrono::seconds(1));
        EXPECT_EQ(logger.write_failures(), before);
        ASSERT_EQ(sel.query().size(), 1u);
        EXPECT_EQ(sel.query()[0].source, "log");
        EXPECT_EQ(sel.query()[0].state, "degraded");
        logger.action("test", "new failure");
        monitor.report_log_degradation(logger, bus, sel, now + std::chrono::seconds(4));
        EXPECT_EQ(sel.query().size(), 1u);
        monitor.report_log_degradation(logger, bus, sel, now + std::chrono::seconds(5));
        EXPECT_EQ(sel.query().size(), 2u);
        bus.stop();
    }
    bmc::SelStore reopened(path);
    EXPECT_EQ(reopened.query().size(), 2u);
    std::filesystem::remove(path);
}
TEST(RuntimeManagement, DroppedBytesAloneTriggerOneBusNotification) {
    const auto path = storage("-drop.sel");
    std::filesystem::remove(path);
    bmc::SelStore sel(path);
    bmc::EventBus bus;
    unsigned received = 0;
    bus.subscribe(bmc::BusEventType::service, [&](const bmc::BusEvent& event) {
        if (event.source == "log") ++received;
    });
    bmc::Monitor monitor;
    HealthLogger logger;
    const auto now = std::chrono::steady_clock::time_point{};
    monitor.report_log_degradation(logger, bus, sel, now);
    EXPECT_TRUE(sel.query().empty());
    logger.dropped = 10;
    monitor.report_log_degradation(logger, bus, sel, now);
    monitor.report_log_degradation(logger, bus, sel, now + std::chrono::seconds(20));
    bus.stop();
    EXPECT_EQ(received, 1u);
    EXPECT_EQ(logger.writes, 0u);
    EXPECT_NE(sel.query()[0].message.find("dropped-bytes=10"), std::string::npos);
    std::filesystem::remove(path);
}
// SEL 已经写不进去，降级只能靠总线与日志：连续两次 poll 也只上报一次。
TEST(RuntimeManagement, SelWriteFailureReportsOnce) {
    bmc::SelStore sel("/dev/full");
    sel.append("cpu", "critical", "failure", std::nullopt, true);
    ASSERT_GT(sel.write_failures(), 0u);
    bmc::EventBus bus;
    bmc::Monitor monitor;
    HealthLogger logger;
    unsigned received = 0;
    bus.subscribe(bmc::BusEventType::service, [&](const bmc::BusEvent& event) {
        if (event.source == "sel") ++received;
    });
    std::vector<bmc::MonitorSensor> sensors;
    bmc::Worker worker(4, 1);
    bmc::FaultRuleEngine rules({});
    bmc::RecoveryPolicyEngine recovery([](const bmc::RecoveryRequest&) { return true; });
    std::atomic_bool failed{false};
    monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, failed);
    monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, failed);
    worker.stop();
    bus.stop();
    EXPECT_EQ(received, 1u);
    EXPECT_EQ(logger.writes, 1u);
}
// ============ 规则状态裁剪：传感器消失后不得残留 ============
TEST(RuntimeManagement, DisappearingSensorsDoNotAccumulateRuleStates) {
    bmc::FaultRuleEngine rules({{"fault", "*", bmc::State::critical, 2, 2, "inspect_device"}});
    for (unsigned index = 0; index < 10000; ++index) {
        const auto id = "sensor-" + std::to_string(index);
        rules.evaluate({id, bmc::State::normal, bmc::State::critical, 95, "sample", 1});
        rules.retain_sensors({id});
        ASSERT_EQ(rules.runtime_size(), 1u);
    }
    rules.retain_sensors({});
    EXPECT_EQ(rules.runtime_size(), 0u);
}
TEST(RuntimeManagement, RetainedSensorKeepsConfirmationAndActiveState) {
    bmc::FaultRuleEngine rules({{"fault", "*", bmc::State::critical, 2, 2, "inspect_device"}});
    const bmc::Event event{"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1};
    EXPECT_TRUE(rules.evaluate(event).empty());
    rules.retain_sensors({"cpu"});
    ASSERT_EQ(rules.evaluate(event).size(), 1u);
    rules.retain_sensors({"cpu"});
    EXPECT_TRUE(rules.evaluate(event).empty());
    rules.retain_sensors({});
    EXPECT_TRUE(rules.evaluate(event).empty());
}
TEST(RuntimeManagement, MonitorPrunesRemovedSensorsWithoutReload) {
    const auto path = storage("-monitor.sel");
    std::filesystem::remove(path);
    bmc::SelStore sel(path);
    bmc::EventBus bus;
    bmc::Worker worker(4, 1);
    bmc::RecoveryPolicyEngine recovery([](const bmc::RecoveryRequest&) { return true; });
    bmc::FaultRuleEngine rules({{"fault", "*", bmc::State::critical, 2, 2, "inspect_device"}});
    rules.evaluate({"removed", bmc::State::normal, bmc::State::critical, 95, "sample", 1});
    HealthLogger logger;
    bmc::Monitor monitor;
    std::vector<bmc::MonitorSensor> sensors;
    std::atomic_bool failed{false};
    monitor.poll(sensors, logger, worker, bus, rules, recovery, sel, failed);
    EXPECT_EQ(rules.runtime_size(), 0u);
    worker.stop(); bus.stop();
    std::filesystem::remove(path);
}
