#include "bmc/core.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

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
