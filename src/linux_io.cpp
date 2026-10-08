#include "bmc/linux_io.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
namespace bmc {
int PosixLinuxIo::open(const std::string& path, int flags) { return ::open(path.c_str(), flags); }
int PosixLinuxIo::ioctl(int descriptor, unsigned long request, void* argument) {
    // 真实 ioctl 是变参；void* 兼作"指针"与"unsigned long"载体，调用点按 ioctl 约定转换。
    return ::ioctl(descriptor, request, argument);
}
ssize_t PosixLinuxIo::read(int descriptor, void* buffer, std::size_t count) { return ::read(descriptor, buffer, count); }
ssize_t PosixLinuxIo::write(int descriptor, const void* buffer, std::size_t count) { return ::write(descriptor, buffer, count); }
LinuxIo& system_io() {
    static PosixLinuxIo instance;
    return instance;
}
}
