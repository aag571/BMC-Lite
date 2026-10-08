#include "bmc/core.hpp"
#include <unistd.h>
#include <utility>

// 文件描述符的 RAII 封装，由 include/bmc/core.hpp 声明。
// 只负责析构即关闭与移动所有权，不提供读写，异常路径下也不会重复关闭同一个编号。
namespace bmc {
// 接管裸描述符的所有权；-1 表示空句柄（默认构造或已被移动走）。
Fd::Fd(int value) : value_(value) {}
// 只关闭仍持有的描述符；析构无法处理 close 的失败，因此不检查返回值。
Fd::~Fd() {
    if (value_ >= 0) {
        ::close(value_);
    }
}
// 转移所有权并把源置成 -1，保证同一个描述符只会被关闭一次。
Fd::Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
// 自赋值检查必须先于 close：否则自移动赋值会关掉自己仍持有的描述符，再把句柄置空。
// 必须先关闭自己原有的句柄再接管：顺序反过来会漏掉自己那个编号，还会立刻关掉刚接管的描述符。
Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        if (value_ >= 0) {
            ::close(value_);
        }
        value_ = std::exchange(other.value_, -1);
    }
    return *this;
}
// 只读取编号，不转移所有权：调用方不得自行 close 这个描述符。
int Fd::get() const { return value_; }
}
