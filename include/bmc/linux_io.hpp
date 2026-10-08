#pragma once
#include <cerrno>
#include <atomic>
#include <cstdint>
#include <string>
#include <unistd.h>
namespace bmc {
// Linux 系统调用的最小可注入接口。生产代码使用 PosixLinuxIo 直通真实 syscall，
// 测试使用 FakeLinuxIo 脚本化返回值，因此 ioctl 解码逻辑无需真实 /dev/i2c-* 或 /dev/gpiochip* 即可覆盖。
class LinuxIo {
public:
    virtual ~LinuxIo() = default;
    virtual int open(const std::string& path, int flags) = 0;
    // Linux 的 ioctl 是变参系统调用：I2C_SLAVE 传整数，I2C_FUNCS/I2C_SMBUS/GPIO_* 传结构体指针。
    // 接口统一为 void*，调用点负责按该 ioctl 约定的类型转换。
    virtual int ioctl(int descriptor, unsigned long request, void* argument) = 0;
    virtual ssize_t read(int descriptor, void* buffer, std::size_t count) = 0;
    virtual ssize_t write(int descriptor, const void* buffer, std::size_t count) = 0;
    // 非虚实现：任何实现都必须真正关闭描述符，并在此留下 errno 供诊断。
    int close(int descriptor) noexcept {
        const int result = ::close(descriptor);
        last_error = result < 0 ? errno : 0;
        return result;
    }
    std::atomic<int> last_error{0};
};
class PosixLinuxIo final : public LinuxIo {
public:
    int open(const std::string& path, int flags) override;
    int ioctl(int descriptor, unsigned long request, void* argument) override;
    ssize_t read(int descriptor, void* buffer, std::size_t count) override;
    ssize_t write(int descriptor, const void* buffer, std::size_t count) override;
};
// 进程级默认实例：无状态，供不关心注入的调用点与既有测试使用。
LinuxIo& system_io();
}
