#include "bmc/linux_io.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>

// LinuxIo 的真实系统调用实现与进程级默认实例，由 include/bmc/linux_io.hpp 声明。
// 每个方法都是一次 syscall 的直接透传，没有重试、缓冲或参数解读，语义与 POSIX 一致。
namespace bmc {
// 透传 ::open：O_CLOEXEC/O_NOFOLLOW 等安全标志由调用点决定（见 write_pwm）。
int PosixLinuxIo::open(const std::string& path, int flags) { return ::open(path.c_str(), flags); }
// request 决定第三个参数的真实类型，调用点必须按该 ioctl 的约定配对，见下方说明。
int PosixLinuxIo::ioctl(int descriptor, unsigned long request, void* argument) {
    // 真实 ioctl 是变参；void* 兼作"指针"与"unsigned long"载体，调用点按 ioctl 约定转换。
    return ::ioctl(descriptor, request, argument);
}
// 透传 ::read；EINTR 与短读等 POSIX 语义原样交给调用点处理。
ssize_t PosixLinuxIo::read(int descriptor, void* buffer, std::size_t count) { return ::read(descriptor, buffer, count); }
// 透传 ::write；短写不在这里补齐，是否需要重试由调用点判断。
ssize_t PosixLinuxIo::write(int descriptor, const void* buffer, std::size_t count) { return ::write(descriptor, buffer, count); }
// 函数局部静态：初始化自带线程安全，实例无状态，整个进程共用一个即可。
LinuxIo& system_io() {
    static PosixLinuxIo instance;
    return instance;
}
}
