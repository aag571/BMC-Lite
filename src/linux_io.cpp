#include "bmc/linux_io.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
namespace bmc {
int PosixLinuxIo::open(const std::string& path, int flags) { return ::open(path.c_str(), flags); }
int PosixLinuxIo::ioctl(int descriptor, unsigned long request, std::uintptr_t argument) { return ::ioctl(descriptor, request, argument); }
ssize_t PosixLinuxIo::read(int descriptor, void* buffer, std::size_t count) { return ::read(descriptor, buffer, count); }
ssize_t PosixLinuxIo::write(int descriptor, const void* buffer, std::size_t count) { return ::write(descriptor, buffer, count); }
int PosixLinuxIo::close(int descriptor) noexcept { return ::close(descriptor); }
}