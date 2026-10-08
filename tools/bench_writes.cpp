// 写入路径基准。输出可解析的键值对，配合 tools/bench-writes.sh 用 strace 精确统计系统调用次数。
// 重点关注 write/fdatasync 的次数，而不是易受机器负载影响的墙钟时间。
#include "bmc/core.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
struct Arguments {
    std::string mode = "both";
    std::filesystem::path directory = "var/bench";
    unsigned records = 100000;
    std::size_t batch = 4096;
    bool sync = false;
    bool important = false;
    unsigned rounds = 1;
};

unsigned positive(const std::string& token, const std::string& option) {
    const auto value = std::stoul(token);
    if (value == 0) {
        throw std::invalid_argument(option + " must be positive");
    }
    return static_cast<unsigned>(value);
}

Arguments parse(int count, char** argv) {
    Arguments result;
    for (int index = 1; index < count; ++index) {
        const std::string option = argv[index];
        if (option == "--sync") {
            result.sync = true;
            continue;
        }
        if (option == "--important") {
            result.important = true;
            continue;
        }
        if (index + 1 >= count) {
            throw std::invalid_argument("missing value for " + option);
        }
        const std::string value = argv[++index];
        if (option == "--mode") {
            result.mode = value;
        } else if (option == "--dir") {
            result.directory = value;
        } else if (option == "--records") {
            result.records = positive(value, option);
        } else if (option == "--batch") {
            result.batch = positive(value, option);
        } else if (option == "--rounds") {
            result.rounds = positive(value, option);
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    return result;
}

// action 记录约 60 字节，与真实运行时的量级一致。
std::string payload(unsigned index) {
    return "entry-" + std::to_string(index) + std::string(40, 'x');
}

template <typename Fn>
double measure_ms(Fn&& body) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto finish = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(finish - start).count();
}

void run_logger(const Arguments& arguments) {
    const auto path = arguments.directory / "bench.log";
    std::filesystem::remove(path);
    std::uint64_t bytes = 0;
    // batch == 1 表示逐条写入，等价于改造前的行为。
    const auto elapsed = measure_ms([&] {
        bmc::Logger logger(path, 64u * 1024 * 1024, 2, arguments.sync, arguments.batch);
        for (unsigned index = 0; index < arguments.records; ++index) {
            const auto line = payload(index);
            bytes += line.size() + 1;
            logger.action("bench", line);
        }
        if (!logger.flush()) {
            throw std::runtime_error("logger flush failed");
        }
    });
    const auto size = std::filesystem::file_size(path);
    std::cout << "mode=logger"
              << " records=" << arguments.records
              << " batch=" << arguments.batch
              << " sync=" << (arguments.sync ? 1 : 0)
              << " payload_bytes=" << bytes
              << " file_bytes=" << size
              << " elapsed_ms=" << elapsed
              << " records_per_sec=" << (arguments.records / (elapsed / 1000.0))
              << '\n';
}

void run_sel(const Arguments& arguments) {
    const auto path = arguments.directory / "bench.sel";
    std::filesystem::remove(path);
    std::uint64_t bytes = 0;
    std::uint64_t write_failures = 0;
    // 栈对象而不是 new：指针间接会阻止 append 被内联，会让基准严重高估成本。
    bmc::SelStore sel(path, 1000000);
    const auto append_ms = measure_ms([&] {
        for (unsigned index = 0; index < arguments.records; ++index) {
            const auto line = payload(index);
            bytes += line.size() + 1;
            // important=true 强制每条记录 write+fdatasync，用于量化"关键证据立即落盘"的代价。
            sel.append("bench", "normal", line, std::nullopt, arguments.important);
        }
    });
    const auto flush_ms = measure_ms([&] {
        if (!sel.flush()) {
            throw std::runtime_error("sel flush failed");
        }
    });
    write_failures = sel.write_failures();

    const auto elapsed = append_ms + flush_ms;
    const auto size = std::filesystem::file_size(path);
    std::cout << "mode=sel"
              << " records=" << arguments.records
              << " important=" << (arguments.important ? 1 : 0)
              << " payload_bytes=" << bytes
              << " file_bytes=" << size
              << " elapsed_ms=" << elapsed
              << " append_ms=" << append_ms
              << " flush_ms=" << flush_ms
              << " records_per_sec=" << (arguments.records / (elapsed / 1000.0))
              << " write_failures=" << write_failures
              << '\n';
}
}
int bench_main(int count, char** argv) {
    try {
        const auto arguments = parse(count, argv);
        std::filesystem::create_directories(arguments.directory);
        if (arguments.mode == "logger" || arguments.mode == "both") {
            run_logger(arguments);
        }
        if (arguments.mode == "sel" || arguments.mode == "both") {
            run_sel(arguments);
        }
        if (arguments.rounds > 1) {
            // 再跑一轮，便于判断差异是否稳定（共享环境的时间抖动可能很大）。
            for (unsigned round = 2; round <= arguments.rounds; ++round) {
                std::cout << "-- round " << round << " --\n";
                if (arguments.mode == "logger" || arguments.mode == "both") {
                    run_logger(arguments);
                }
                if (arguments.mode == "sel" || arguments.mode == "both") {
                    run_sel(arguments);
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bench-writes: " << error.what() << '\n';
        return 1;
    }
}
#ifndef BENCH_WRITES_NO_MAIN
int main(int count, char** argv) { return bench_main(count, argv); }
#endif
