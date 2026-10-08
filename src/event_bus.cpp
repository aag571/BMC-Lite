#include "bmc/core.hpp"
#include <stdexcept>
#include <thread>

namespace bmc {
EventBus::EventBus(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) throw std::invalid_argument("zero event bus capacity");
    thread_ = std::thread(&EventBus::dispatch, this);
}
EventBus::~EventBus() { stop(); }
std::uint64_t EventBus::subscribe(BusEventType type, Handler handler) {
    if (!handler) throw std::invalid_argument("empty event handler");
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("event bus stopped");
    const auto id = next_id_++;
    subscriptions_.push_back({id, type, std::move(handler)});
    return id;
}
bool EventBus::publish(BusEvent event) {
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    event.sequence = next_sequence_++;
    if (queue_.size() >= capacity_) { ++dropped_; return false; }
    queue_.push_back(std::move(event));
    wake_.notify_one();
    return true;
}
void EventBus::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}
std::uint64_t EventBus::dropped() const { std::lock_guard lock(mutex_); return dropped_; }
void EventBus::dispatch() {
    for (;;) {
        BusEvent event;
        std::vector<Handler> handlers;
        { std::unique_lock lock(mutex_);
          wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
          if (queue_.empty() && stopping_) return;
          event = std::move(queue_.front()); queue_.pop_front();
          for (const auto& subscription : subscriptions_)
              if (subscription.type == event.type) handlers.push_back(subscription.handler);
        }
        for (const auto& handler : handlers) { try { handler(event); } catch (...) {} }
    }
}
}
