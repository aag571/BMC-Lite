#include "bmc/core.hpp"
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

// Worker 线程池：有界优先队列加固定数量工作线程，由 include/bmc/core.hpp 声明。
// 队列满或已停止时 submit 拒绝任务；任务在锁外执行，异常只计入统计。
namespace bmc {
// 容量必须 >=1、线程数取 1..64：容量为 0 的队列永远拒绝任务，线程过多属于配置失控。
Worker::Worker(std::size_t capacity, std::size_t threads) : capacity_(capacity) {
    // --worker-threads 在命令行侧只校验 1..1000000，64 这个线程上限只在这里兜底。
    if (capacity == 0 || threads == 0 || threads > 64) {
        throw std::invalid_argument("invalid worker capacity or thread count");
    }
    try {
        // 线程创建可能因资源不足抛异常，此时已启动的线程必须回收，否则构造失败会泄漏线程。
        for (std::size_t index = 0; index < threads; ++index) threads_.emplace_back(&Worker::run, this);
    } catch (...) {
        stop();
        throw;
    }
}
// 析构即 stop()，保证所有工作线程在成员析构前退出。
Worker::~Worker() { stop(); }
// 一次持锁完成校验与插入：priority 越大越先执行，同优先级保持 FIFO。
bool Worker::submit(std::function<void()> task, int priority) {
    std::lock_guard lock(mutex_);
    // 空任务、已停止、队列已满都返回 false 并计入 rejected_，让调用方能区分丢弃与执行失败。
    if (!task || stopping_ || queue_.size() >= capacity_) {
        ++rejected_;
        return false;
    }
    // 找到第一个优先级低于新任务的元素并插到它前面，从而维持降序与同优先级 FIFO。
    const auto position = std::find_if(queue_.begin(), queue_.end(), [priority](const Task& queued) { return queued.priority < priority; });
    queue_.insert(position, Task{std::move(task), priority});
    ++accepted_;
    wake_.notify_one();
    return true;
}
// 幂等：置位停止标志、唤醒全部线程后 join；返回后不会再有任务在运行。
void Worker::stop() {
    {
        // 只包住标志置位：持锁 join 会让等锁的工作线程永远退不出来。
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    // 线程仍在访问本对象成员，必须 join 而不能 detach。
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
}
// 持锁取一致快照：计数与队列长度必须来自同一时刻，否则监控数字会自相矛盾。
Worker::Stats Worker::stats() const {
    std::lock_guard lock(mutex_);
    return {accepted_, completed_, failed_, rejected_, queue_.size(), running_};
}
// 工作线程主循环：等待 -> 取出任务 -> 释放锁执行 -> 回写统计。
void Worker::run() {
    // 取任务时持锁，执行时释放锁；退出仍处理队列中的已接受任务。
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            // 谓词同时覆盖虚假唤醒与退出条件；停止且队列已空才退出。
            // 因此 stop() 之后队列里已接受的任务仍会被执行完，而不是被丢弃。
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) {
                return;
            }
            task = std::move(queue_.front().execute);
            queue_.pop_front();
            ++running_;
        }
        // 锁必须在任务执行前释放：任务可能再次进入 Worker（提交或查统计），持锁调用既会自死锁，
        // 也会把所有工作线程串行化。
        bool failed = false;
        // 任务异常只体现为 failed_ 计数：线程池不能因为某个任务抛出就少一个线程。
        try { task(); } catch (...) { failed = true; }
        {
            std::lock_guard lock(mutex_);
            // completed_ 含成功与失败，failed_ 是其子集：成功数 = completed_ - failed_。
            --running_;
            ++completed_;
            if (failed) ++failed_;
        }
    }
}
}
