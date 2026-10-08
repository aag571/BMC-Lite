#include "bmc/core.hpp"
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bmc {
Worker::Worker(std::size_t capacity, std::size_t threads) : capacity_(capacity) {
    if (capacity == 0 || threads == 0 || threads > 64) {
        throw std::invalid_argument("invalid worker capacity or thread count");
    }
    try {
        for (std::size_t index = 0; index < threads; ++index) threads_.emplace_back(&Worker::run, this);
    } catch (...) {
        stop();
        throw;
    }
}
Worker::~Worker() { stop(); }
bool Worker::submit(std::function<void()> task, int priority) {
    std::lock_guard lock(mutex_);
    if (!task || stopping_ || queue_.size() >= capacity_) {
        ++rejected_;
        return false;
    }
    const auto position = std::find_if(queue_.begin(), queue_.end(), [priority](const Task& queued) { return queued.priority < priority; });
    queue_.insert(position, Task{std::move(task), priority});
    ++accepted_;
    wake_.notify_one();
    return true;
}
void Worker::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
}
Worker::Stats Worker::stats() const {
    std::lock_guard lock(mutex_);
    return {accepted_, completed_, failed_, rejected_, queue_.size(), running_};
}
void Worker::run() {
    // 取任务时持锁，执行时释放锁；退出仍处理队列中的已接受任务。
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) {
                return;
            }
            task = std::move(queue_.front().execute);
            queue_.pop_front();
            ++running_;
        }
        bool failed = false;
        try { task(); } catch (...) { failed = true; }
        {
            std::lock_guard lock(mutex_);
            --running_;
            ++completed_;
            if (failed) ++failed_;
        }
    }
}
}
