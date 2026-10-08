#include "bmc/monitor.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

namespace {
class HealthLogger final : public bmc::EventLogger {
public:
    std::uint64_t failures = 0, dropped = 0;
    unsigned writes = 0;
    void write(const bmc::Event&) override { ++writes; }
    void action(const std::string&, const std::string&) override { ++writes; }
    std::uint64_t write_failures() const override { return failures; }
    std::uint64_t dropped_bytes() const override { return dropped; }
};
std::filesystem::path storage(const std::string& suffix) {
    return std::filesystem::temp_directory_path() / ("bmc-runtime-management-" + std::to_string(::getpid()) + suffix);
}
}
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
