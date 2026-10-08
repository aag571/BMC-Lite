#include "bmc/core.hpp"
#include <stdexcept>
#include <thread>

// 进程内异步事件分发：有界队列加一条分发线程，由 include/bmc/core.hpp 声明。
// publish 永不阻塞（可能被采样或网络线程调用），队列满即丢弃并计数。
namespace bmc {
// 容量为 0 时每条事件都会被丢弃，属于配置错误，直接拒绝。
EventBus::EventBus(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) throw std::invalid_argument("zero event bus capacity");
    // 校验通过后才启动线程，避免构造失败时还要回收一个已经跑起来的线程。
    thread_ = std::thread(&EventBus::dispatch, this);
}
// 析构即 stop()，保证分发线程在成员析构前结束。
EventBus::~EventBus() { stop(); }
// 注册订阅并返回 id；空 handler 与已停止的总线都拒绝，避免留下永远不会生效的订阅。
std::uint64_t EventBus::subscribe(BusEventType type, Handler handler) {
    if (!handler) throw std::invalid_argument("empty event handler");
    std::lock_guard lock(mutex_);
    // 分发线程已经不会再处理队列，此时注册没有意义。
    if (stopping_) throw std::runtime_error("event bus stopped");
    // id 单调递增且不复用，便于在日志里区分不同订阅（当前不支持取消订阅）。
    const auto id = next_id_++;
    subscriptions_.push_back({id, type, std::move(handler)});
    return id;
}
// 入队成功返回 true；队列满或已停止返回 false，两种情况都绝不阻塞发布者。
bool EventBus::publish(BusEvent event) {
    std::lock_guard lock(mutex_);
    // 停止时直接拒绝但不计 dropped_：这是主动关闭，不是运行期丢失。
    if (stopping_) return false;
    // 序号在丢弃判断之前分配：它标识发布顺序而非送达顺序，被丢弃的事件同样占用序号。
    event.sequence = next_sequence_++;
    // 队列满即丢弃并计数：发布者可能是采样线程，阻塞它会让整轮采集停摆。
    if (queue_.size() >= capacity_) { ++dropped_; return false; }
    queue_.push_back(std::move(event));
    wake_.notify_one();
    return true;
}
// 置停止标志、唤醒分发线程后 join：分发线程会先处理完队列，
// join 保证 stop 返回时回调已全部结束。
void EventBus::stop() {
    // 只包住标志置位：持锁 join 会与等锁的分发线程形成死锁。
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}
// 返回运行期丢弃计数，其中不含停止时被拒的发布。
std::uint64_t EventBus::dropped() const { std::lock_guard lock(mutex_); return dropped_; }
// 分发线程主循环：持锁取出事件并收集订阅者，解锁后逐个回调。
void EventBus::dispatch() {
    for (;;) {
        BusEvent event;
        std::vector<Handler> handlers;
        { std::unique_lock lock(mutex_);
          wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
          // 先排空队列再退出：已接受的事件不能因为关闭而静默丢失。
          if (queue_.empty() && stopping_) return;
          event = std::move(queue_.front()); queue_.pop_front();
          // 在锁内只复制 handler：回调可能在锁外再次 publish 或 subscribe，持锁回调会自死锁。
          for (const auto& subscription : subscriptions_)
              if (subscription.type == event.type) handlers.push_back(subscription.handler);
        }
        // 订阅者异常一律吞掉：一个处理器失败不能杀死分发线程，也不能影响其他订阅者。
        for (const auto& handler : handlers) { try { handler(event); } catch (...) {} }
    }
}
}
