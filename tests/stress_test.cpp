#include "bmc/core.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// 调度与总线的竞态压力验证。它独立成可执行文件（CMake 目标 bmc_stress，CTest 用例
// scheduler_stress，带 stress 标签），因为它是 TSan / Valgrind 与压力测试的专用入口：
// 不计时、不依赖 gtest，失败即以非零退出码结束。
// 断言两条守恒关系：
//   1. worker 记账守恒：executed == accepted、completed == accepted，结束后
//      running/queued/failed 全为 0，且 accepted + rejected == 生产者提交总数 80000。
//   2. 总线守恒：delivered == published，且 published + dropped == 生产者发布总数 40000。
// 线程数与提交次数写死，是为了让 TSan 下的调度交错可复现。
int main() {
    bmc::Worker worker(256, 4);
    std::atomic<unsigned> executed = 0;
    std::atomic<unsigned> accepted = 0;
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < 8; ++producer) {
        producers.emplace_back([&] {
            for (unsigned index = 0; index < 10000; ++index) {
                if (worker.submit([&] { ++executed; }, static_cast<int>(index % 4))) ++accepted;
            }
        });
    }
    for (auto& producer : producers) producer.join();
    worker.stop();
    const auto stats = worker.stats();
    if (executed != accepted || stats.completed != accepted || stats.running || stats.queued || stats.failed || stats.accepted + stats.rejected != 80000) {
        throw std::runtime_error("worker stress accounting failed");
    }
    bmc::EventBus bus(256);
    std::atomic<unsigned> delivered = 0;
    std::atomic<unsigned> published = 0;
    bus.subscribe(bmc::BusEventType::service, [&](const bmc::BusEvent&) { ++delivered; });
    producers.clear();
    for (unsigned producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&] {
            for (unsigned index = 0; index < 10000; ++index) {
                if (bus.publish({bmc::BusEventType::service, "stress", "sample", std::nullopt})) ++published;
            }
        });
    }
    for (auto& producer : producers) producer.join();
    bus.stop();
    if (delivered != published || published + bus.dropped() != 40000) throw std::runtime_error("bus stress accounting failed");
    std::cout << "worker accepted=" << accepted << " rejected=" << stats.rejected
              << " bus delivered=" << delivered << " dropped=" << bus.dropped() << '\n';
}
