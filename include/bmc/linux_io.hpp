#pragma once
#include <cstdint>
#include <string>
#include <sys/types.h>
namespace bmc {
class LinuxIo {
public:
    virtual ~LinuxIo() = default;
    virtual int open(const std::string& path, int flags) = 0;
    virtual int ioctl(int descriptor, unsigned long request, std::uintptr_t argument) = 0;
    virtual ssize_t read(int descriptor, void* buffer, std::size_t count) = 0;
    virtual ssize_t write(int descriptor, const void* buffer, std::size_t count) = 0;
    virtual int close(int descriptor) noexcept = 0;
};
class PosixLinuxIo final : public LinuxIo {
public:
    int open(const std::string& path, int flags) override;
    int ioctl(int descriptor, unsigned long request, std::uintptr_t argument) override;
    ssize_t read(int descriptor, void* buffer, std::size_t count) override;
    ssize_t write(int descriptor, const void* buffer, std::size_t count) override;
    int close(int descriptor) noexcept override;
};
}