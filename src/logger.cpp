#include "bmc/core.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace bmc {
namespace {
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(milliseconds);
}
}
Logger::Logger(std::filesystem::path path, std::uintmax_t limit, unsigned keep)
    : path_(std::move(path)), limit_(limit), keep_(keep) {
    if (limit == 0 || keep == 0 || keep > 100) {
        throw std::invalid_argument("invalid rotation policy");
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
}
void Logger::rotate(std::size_t incoming) {
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) + incoming <= limit_) {
        return;
    }
    for (unsigned index = keep_; index > 0; --index) {
        const auto destination = std::filesystem::path(path_.string() + "." + std::to_string(index));
        const auto source = index == 1 ? path_ : std::filesystem::path(path_.string() + "." + std::to_string(index - 1));
        if (std::filesystem::exists(destination)) {
            std::filesystem::remove(destination);
        }
        if (std::filesystem::exists(source)) {
            std::filesystem::rename(source, destination);
        }
    }
}
void Logger::append(const std::string& line) {
    std::lock_guard lock(mutex_);
    rotate(line.size() + 1);
    std::ofstream output(path_, std::ios::app);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << line << '\n';
    output.flush();
}
void Logger::write(const Event& event) {
    std::ostringstream output;
    output << std::setprecision(17) << "{\"time_ms\":" << timestamp()
           << ",\"sensor\":\"" << escape(event.id)
           << "\",\"before\":\"" << name(event.before)
           << "\",\"state\":\"" << name(event.after)
           << "\",\"value\":";
    if (event.value && std::isfinite(*event.value)) {
        output << *event.value;
    } else {
        output << "null";
    }
    output << ",\"reason\":\"" << escape(event.reason) << "\",\"sequence\":" << event.sequence << '}';
    append(output.str());
}
void Logger::action(const std::string& id, const std::string& result) {
    append("{\"time_ms\":" + timestamp() + ",\"sensor\":\"" + escape(id) + "\",\"action\":\"" + escape(result) + "\"}");
}
}
