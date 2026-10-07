#include "bmc/core.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <limits>
#include <future>
#include <mutex>
#include <unistd.h>

namespace {
bmc::Config policy() {
    bmc::Config config;
    config.id = "cpu";
    config.backend = "mock";
    config.path = "40,75,95,err";
    return config;
}
std::filesystem::path temporary(const std::string& suffix) {
    return std::filesystem::temp_directory_path() / ("bmc-test-" + std::to_string(::getpid()) + suffix);
}
TEST(Engine, DebouncesCriticalAndUsesInclusiveBoundary) {
    bmc::Engine engine(policy());
    EXPECT_FALSE(engine.update(90));
    EXPECT_FALSE(engine.update(90));
    const auto event = engine.update(90);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->after, bmc::State::critical);
    EXPECT_EQ(event->before, bmc::State::normal);
    EXPECT_EQ(event->sequence, 1u);
    EXPECT_FALSE(engine.update(100));
}
TEST(Engine, HysteresisRequiresCrossingRecoveryBoundary) {
    auto config = policy();
    config.debounce = 1;
    bmc::Engine engine(config);
    ASSERT_TRUE(engine.update(90));
    EXPECT_FALSE(engine.update(87));
    ASSERT_TRUE(engine.update(86));
    EXPECT_EQ(engine.state(), bmc::State::warning);
    EXPECT_FALSE(engine.update(67));
    ASSERT_TRUE(engine.update(66));
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
TEST(Engine, AlternatingValuesResetDebounce) {
    bmc::Engine engine(policy());
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        EXPECT_FALSE(engine.update(95));
        EXPECT_FALSE(engine.update(40));
    }
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
TEST(Engine, InvalidSamplesBecomeUnavailableAndRecover) {
    bmc::Engine engine(policy());
    EXPECT_FALSE(engine.update(std::nullopt));
    EXPECT_FALSE(engine.update(std::numeric_limits<double>::infinity()));
    const auto event = engine.update(std::numeric_limits<double>::quiet_NaN());
    ASSERT_TRUE(event);
    EXPECT_EQ(event->after, bmc::State::unavailable);
    EXPECT_FALSE(event->value);
    EXPECT_FALSE(engine.update(40));
    EXPECT_FALSE(engine.update(40));
    ASSERT_TRUE(engine.update(40));
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
TEST(Engine, FailedReadingBreaksPendingTransition) {
    bmc::Engine engine(policy());
    EXPECT_FALSE(engine.update(95));
    EXPECT_FALSE(engine.update(95));
    EXPECT_FALSE(engine.update(std::nullopt));
    EXPECT_FALSE(engine.update(95));
    EXPECT_FALSE(engine.update(95));
    EXPECT_TRUE(engine.update(95));
}
TEST(Engine, LowDirectionAndRecovery) {
    auto config = policy();
    config.high = false;
    config.warning = 1500;
    config.critical = 500;
    config.hysteresis = 100;
    config.debounce = 1;
    bmc::Engine engine(config);
    ASSERT_TRUE(engine.update(500));
    EXPECT_EQ(engine.state(), bmc::State::critical);
    EXPECT_FALSE(engine.update(600));
    ASSERT_TRUE(engine.update(601));
    EXPECT_EQ(engine.state(), bmc::State::warning);
    EXPECT_FALSE(engine.update(1600));
    ASSERT_TRUE(engine.update(1601));
    EXPECT_EQ(engine.state(), bmc::State::normal);
}
TEST(Config, RejectsBrokenThresholdsAndWindows) {
    auto config = policy();
    config.critical = config.warning;
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.debounce = 0;
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
    config = policy();
    config.scale = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(bmc::validate(config), std::invalid_argument);
}
TEST(Reader, MockCyclesAndScales) {
    auto config = policy();
    config.scale = 2;
    auto reader = bmc::make_reader(config);
    EXPECT_EQ(reader->read(), 80);
    EXPECT_EQ(reader->read(), 150);
    EXPECT_EQ(reader->read(), 190);
    EXPECT_FALSE(reader->read());
    EXPECT_EQ(reader->read(), 80);
}
TEST(Reader, SysfsRejectsMalformedAndMissingData) {
    const auto path = temporary("-sensor");
    auto config = policy();
    config.backend = "sysfs";
    config.path = path.string();
    config.scale = 0.001;
    auto reader = bmc::make_reader(config);
    EXPECT_FALSE(reader->read());
    { std::ofstream output(path); output << "72000\n"; }
    EXPECT_EQ(reader->read(), 72);
    { std::ofstream output(path); output << "72000 garbage\n"; }
    EXPECT_FALSE(reader->read());
    std::filesystem::remove(path);
}
TEST(Logger, EscapesJsonAndRotates) {
    const auto path = temporary("-log");
    bmc::Logger logger(path, 100, 2);
    logger.action("sensor\"\n", "first");
    logger.action("sensor", "second");
    logger.action("sensor", "third");
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(path.string() + ".1"));
    EXPECT_EQ(bmc::escape("a\"\n\\"), "a\\\"\\n\\\\");
    for (const auto& suffix : {"", ".1", ".2"}) {
        std::filesystem::remove(path.string() + suffix);
    }
}
TEST(Worker, DrainsAndRejectsAfterStopping) {
    std::atomic<unsigned> completed = 0;
    bmc::Worker worker(20);
    for (unsigned iteration = 0; iteration < 10; ++iteration) {
        EXPECT_TRUE(worker.submit([&completed] { ++completed; }));
    }
    worker.stop();
    EXPECT_EQ(completed.load(), 10u);
    EXPECT_FALSE(worker.submit([] {}));
}
TEST(Config, RejectsDuplicateAndNegativeWindow) {
    const auto path = temporary("-config");
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 -1 3 -\n"; }
    EXPECT_THROW(bmc::load_config(path), std::invalid_argument);
    { std::ofstream output(path); output << "cpu mock 40 1 high 70 90 3 3 3 -\ncpu mock 40 1 high 70 90 3 3 3 -\n"; }
    EXPECT_THROW(bmc::load_config(path), std::invalid_argument);
    std::filesystem::remove(path);
}
TEST(Pwm, ValidatesAndWritesConfiguredFile) {
    const auto path = temporary("-pwm");
    { std::ofstream output(path); output << "0\n"; }
    EXPECT_THROW(bmc::write_pwm(path.string(), 256), std::invalid_argument);
    bmc::write_pwm(path.string(), 255);
    std::ifstream input(path);
    unsigned value = 0;
    input >> value;
    EXPECT_EQ(value, 255u);
    std::filesystem::remove(path);
}
TEST(Device, RegistryProvidesLifecycleAndInventory) {
    auto config = policy();
    auto device = bmc::make_device(config);
    EXPECT_EQ(device->state(), bmc::DeviceState::closed);
    EXPECT_TRUE(device->open());
    EXPECT_EQ(device->state(), bmc::DeviceState::ready);
    EXPECT_TRUE(device->probe());
    EXPECT_EQ(device->read_value(), 75);
    bmc::DeviceRegistry registry;
    registry.add(std::move(device));
    ASSERT_NE(registry.find("cpu"), nullptr);
    ASSERT_EQ(registry.inventory().size(), 1u);
    EXPECT_TRUE(registry.open_all());
    registry.close_all();
    EXPECT_EQ(registry.find("cpu")->state(), bmc::DeviceState::closed);
}
TEST(Device, RegistryRejectsDuplicateIds) {
    auto config = policy();
    bmc::DeviceRegistry registry;
    registry.add(bmc::make_device(config));
    EXPECT_THROW(registry.add(bmc::make_device(config)), std::invalid_argument);
}
TEST(Device, SysfsAdapterReportsDegradedState) {
    auto config = policy();
    config.backend = "sysfs";
    config.path = "/definitely/missing/bmc-sensor";
    auto device = bmc::make_device(config);
    EXPECT_FALSE(device->open());
    EXPECT_EQ(device->state(), bmc::DeviceState::failed);
    EXPECT_FALSE(device->probe());
}
TEST(EventBus, DeliversFilteredEventsAndSequencesThem) {
    bmc::EventBus bus(8);
    std::mutex mutex;
    std::vector<bmc::BusEvent> received;
    bus.subscribe(bmc::BusEventType::sensor_state, [&mutex, &received](const bmc::BusEvent& event) {
        std::lock_guard lock(mutex); received.push_back(event);
    });
    EXPECT_TRUE(bus.publish({bmc::BusEventType::configuration, "cfg", "ignored", std::nullopt}));
    EXPECT_TRUE(bus.publish({bmc::BusEventType::sensor_state, "cpu", "critical", 95.0}));
    bus.stop();
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].source, "cpu");
    EXPECT_EQ(received[0].sequence, 2u);
}
TEST(EventBus, DropsWhenFullAndRejectsAfterStop) {
    bmc::EventBus bus(1);
    std::promise<void> blocker;
    auto future = blocker.get_future();
    bus.subscribe(bmc::BusEventType::service, [&future](const bmc::BusEvent&) { future.wait(); });
    EXPECT_TRUE(bus.publish({bmc::BusEventType::service, "s", "first", std::nullopt}));
    for (unsigned index = 0; index < 100; ++index) {
        bus.publish({bmc::BusEventType::service, "s", "queued", std::nullopt});
    }
    EXPECT_GT(bus.dropped(), 0u);
    blocker.set_value();
    bus.stop();
    EXPECT_FALSE(bus.publish({bmc::BusEventType::service, "s", "stopped", std::nullopt}));
}
TEST(Rules, ActivatesAfterConfirmationsAndClearsSeparately) {
    bmc::FaultRule rule{"cpu", "cpu", bmc::State::critical, 2, 2, "increase_fan"};
    bmc::FaultRuleEngine engine({rule});
    bmc::Event first{"cpu", bmc::State::normal, bmc::State::critical, 95, "threshold", 1};
    EXPECT_TRUE(engine.evaluate(first).empty());
    auto activated = engine.evaluate(first);
    ASSERT_EQ(activated.size(), 1u);
    EXPECT_TRUE(activated[0].active);
    bmc::Event normal{"cpu", bmc::State::critical, bmc::State::normal, 40, "threshold", 2};
    EXPECT_TRUE(engine.evaluate(normal).empty());
    auto cleared = engine.evaluate(normal);
    ASSERT_EQ(cleared.size(), 1u);
    EXPECT_FALSE(cleared[0].active);
    EXPECT_EQ(cleared[0].action, "clear");
}
TEST(Recovery, AppliesCooldownRetriesAndReset) {
    unsigned calls = 0;
    bmc::RecoveryPolicyEngine engine([&](const bmc::RecoveryRequest&) { ++calls; return calls >= 2; }, std::chrono::hours(1), 3, 1);
    auto first = engine.submit({"r", "cpu", "increase_fan", 1});
    ASSERT_TRUE(first); EXPECT_TRUE(first->accepted); EXPECT_TRUE(first->success); EXPECT_EQ(first->attempts, 2u);
    auto blocked = engine.submit({"r", "cpu", "increase_fan", 2});
    ASSERT_TRUE(blocked); EXPECT_FALSE(blocked->accepted); EXPECT_EQ(blocked->detail, "cooldown");
    engine.reset("cpu");
    auto second = engine.submit({"r", "cpu", "increase_fan", 3});
    ASSERT_TRUE(second); EXPECT_TRUE(second->accepted);
}
TEST(Recovery, RejectsInvalidPolicy) {
    EXPECT_THROW(bmc::RecoveryPolicyEngine(nullptr), std::invalid_argument);
    EXPECT_THROW(bmc::RecoveryPolicyEngine([](const bmc::RecoveryRequest&) { return true; }, std::chrono::seconds(1), 0), std::invalid_argument);
}
TEST(Sel, PersistsSequencesAndLimitsRecords) {
    const auto path = temporary("-sel.db");
    { bmc::SelStore store(path, 2); EXPECT_EQ(store.append("cpu", "critical", "hot", 95), 1u); EXPECT_EQ(store.append("cpu", "normal", "clear"), 2u); EXPECT_EQ(store.append("fan", "failed", "stalled"), 3u); EXPECT_EQ(store.query().size(), 2u); }
    bmc::SelStore restored(path, 2);
    ASSERT_EQ(restored.query().size(), 2u);
    EXPECT_EQ(restored.query()[0].id, 2u);
    EXPECT_EQ(restored.next_id(), 4u);
    std::filesystem::remove(path);
}
TEST(Gpio, RejectsMalformedPathsBeforeOpeningHardware) {
    auto config = policy();
    config.backend = "gpio";
    for (const auto& path : {"/dev/gpiochip0", "/dev/gpiochip0,-1", "/dev/gpiochip0,1,invalid", "/dev/gpiochip0,4294967296"}) {
        config.path = path;
        EXPECT_THROW(bmc::make_reader(config), std::invalid_argument);
    }
}
TEST(Gpio, MissingDeviceCanRetryAndCloseSafely) {
    auto config = policy();
    config.backend = "gpio";
    config.path = "/definitely/missing/gpiochip,0";
    auto device = bmc::make_device(config);
    EXPECT_FALSE(device->open());
    EXPECT_EQ(device->state(), bmc::DeviceState::failed);
    EXPECT_FALSE(device->read_value());
    device->close();
    EXPECT_EQ(device->state(), bmc::DeviceState::closed);
}
TEST(Worker, IsolatesExceptionsAndReportsConcurrentCompletion) {
    bmc::Worker worker(100, 4);
    std::atomic<unsigned> count = 0;
    EXPECT_TRUE(worker.submit([] { throw std::runtime_error("task failure"); }));
    for (unsigned index = 0; index < 50; ++index) EXPECT_TRUE(worker.submit([&count] { ++count; }));
    worker.stop();
    EXPECT_EQ(count.load(), 50u);
    const auto stats = worker.stats();
    EXPECT_EQ(stats.accepted, 51u);
    EXPECT_EQ(stats.completed, 51u);
    EXPECT_EQ(stats.failed, 1u);
    EXPECT_EQ(stats.running, 0u);
    EXPECT_EQ(stats.queued, 0u);
}
TEST(Worker, OrdersQueuedTasksByPriorityAndPreservesTies) {
    bmc::Worker worker(8);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future();
    worker.submit([&] { entered.set_value(); released.wait(); });
    entered.get_future().wait();
    std::vector<int> order;
    worker.submit([&] { order.push_back(1); }, 0);
    worker.submit([&] { order.push_back(2); }, 10);
    worker.submit([&] { order.push_back(3); }, 10);
    release.set_value();
    worker.stop();
    EXPECT_EQ(order, (std::vector<int>{2, 3, 1}));
}
TEST(FaultInjection, EventSubscriberExceptionDoesNotStopDelivery) {
    bmc::EventBus bus;
    std::atomic<unsigned> received = 0;
    bus.subscribe(bmc::BusEventType::service, [](const bmc::BusEvent&) {
        throw std::runtime_error("injected subscriber failure");
    });
    bus.subscribe(bmc::BusEventType::service, [&received](const bmc::BusEvent&) { ++received; });
    ASSERT_TRUE(bus.publish({bmc::BusEventType::service, "service", "injected", std::nullopt}));
    bus.stop();
    EXPECT_EQ(received.load(), 1u);
}
TEST(FaultInjection, RecoveryExecutorExceptionExhaustsAttempts) {
    unsigned attempts = 0;
    bmc::RecoveryPolicyEngine engine([&attempts](const bmc::RecoveryRequest&) -> bool {
        ++attempts;
        throw std::runtime_error("injected recovery failure");
    }, std::chrono::seconds(0), 3);
    const auto result = engine.submit({"rule", "cpu", "increase_fan", 1});
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->accepted);
    EXPECT_FALSE(result->success);
    EXPECT_EQ(result->attempts, 3u);
    EXPECT_EQ(attempts, 3u);
}
TEST(Rules, WildcardSensorsHaveIndependentState) {
    bmc::FaultRuleEngine engine({{"all", "*", bmc::State::critical, 1, 1, "inspect_device"}});
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::normal, bmc::State::critical, 95, "sample", 1}).size(), 1u);
    EXPECT_EQ(engine.evaluate({"fan", bmc::State::normal, bmc::State::critical, 0, "sample", 1}).size(), 1u);
    EXPECT_EQ(engine.evaluate({"cpu", bmc::State::critical, bmc::State::normal, 40, "sample", 2}).size(), 1u);
    EXPECT_TRUE(engine.evaluate({"fan", bmc::State::critical, bmc::State::critical, 0, "sample", 2}).empty());
}
}
