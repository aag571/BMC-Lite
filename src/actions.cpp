#include "bmc/core.hpp"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

// 恢复动作的真实硬件写入：把 PWM 值写进 sysfs 设备节点。
// write_pwm 由 include/bmc/core.hpp 声明（Action 接口在 include/bmc/action.hpp），
// 本文件只负责写入与失败上报。
namespace bmc {
namespace {
// 用当前 errno 构造异常；集中一处，确保每处失败都带上 errno 与可读的操作名。
[[noreturn]] void system_failure(const std::string& message) {
    throw std::system_error(errno, std::generic_category(), message);
}
// 只为"写失败也要关掉描述符"提供作用域退出，不占用 Fd（Fd 的析构走真实 ::close）。
class DescriptorGuard {
public:
    // 借用注入的 io（生命周期必须长于本对象）并接管 descriptor。
    DescriptorGuard(LinuxIo& io, int descriptor) noexcept : io_(io), descriptor_(descriptor) {}
    // 禁止复制：两份 guard 会在析构时关闭同一个描述符。
    DescriptorGuard(const DescriptorGuard&) = delete;
    DescriptorGuard& operator=(const DescriptorGuard&) = delete;
    // 正常返回与异常展开都会走到这里；close 失败无处上报，因此忽略返回值。
    ~DescriptorGuard() { io_.close(descriptor_); }
private:
    LinuxIo& io_;
    int descriptor_;
};
}
// 唯一的硬件写路径：sysfs 的 PWM 属性接受十进制文本，255 表示满速。
void write_pwm(const std::string& path, unsigned value, LinuxIo& io) {
    // 超出 8 位范围 sysfs 会拒绝，提前抛错比留下一个 EINVAL 更能指明原因。
    if (value > 255) {
        throw std::invalid_argument("PWM value out of range");
    }
    // O_CLOEXEC：动作可能在其他线程 fork/exec 时被继承，写句柄不能泄漏给子进程。
    // O_NOFOLLOW：拒绝末端符号链接；动作路径来自配置，这是防止写入被改道的最后一道防线。
    const int descriptor = io.open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        system_failure("open PWM");
    }
    // 从这里起的每条失败路径都由 guard 关闭描述符，不必在各分支手动 close。
    const DescriptorGuard guard(io, descriptor);
    // 写成十进制文本加换行，与内核 PWM sysfs 属性的输入格式一致。
    const std::string text = std::to_string(value) + "\n";
    const auto count = io.write(descriptor, text.data(), text.size());
    if (count < 0) {
        system_failure("write PWM");
    }
    // 短写不重试：sysfs 的每次 write 都是一次独立的属性写入，补写剩余字节会被当成新的一次写。
    if (static_cast<std::size_t>(count) != text.size()) {
        throw std::runtime_error("short PWM write");
    }
}
}
