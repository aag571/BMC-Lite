#include "bmc/core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bmc {
namespace {
double number(const std::string& token) {
    std::size_t consumed = 0;
    const double value = std::stod(token, &consumed);
    if (consumed != token.size() || !std::isfinite(value)) {
        throw std::invalid_argument("invalid number: " + token);
    }
    return value;
}
}
SelStore::SelStore(std::filesystem::path path, std::size_t max_records) : path_(std::move(path)), max_records_(max_records) {
    if (max_records_ == 0) throw std::invalid_argument("zero SEL capacity");
    if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
    load();
}
void SelStore::load() {
    std::lock_guard lock(mutex_);
    std::ifstream input(path_);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line); SelRecord record; std::string value;
        if (!(fields >> record.id >> record.time_ms >> std::quoted(record.source) >> std::quoted(record.state) >> std::quoted(record.message) >> value)) continue;
        if (value != "null") { try { record.value = number(value); } catch (...) { continue; } }
        records_.push_back(std::move(record));
        next_id_ = std::max(next_id_, records_.back().id + 1);
    }
    if (records_.size() > max_records_) records_.erase(records_.begin(), records_.end() - static_cast<std::ptrdiff_t>(max_records_));
}
std::uint64_t SelStore::append(const std::string& source, const std::string& state, const std::string& message, std::optional<double> value) {
    std::lock_guard lock(mutex_);
    const auto id = next_id_++; const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    records_.push_back({id, time, source, state, message, value});
    if (records_.size() > max_records_) records_.erase(records_.begin());
    std::ofstream output(path_, std::ios::app); output.exceptions(std::ios::badbit | std::ios::failbit);
    output << id << ' ' << time << ' ' << std::quoted(source) << ' ' << std::quoted(state) << ' ' << std::quoted(message) << ' ';
    if (value && std::isfinite(*value)) output << std::setprecision(17) << *value; else output << "null";
    output << '\n'; output.flush(); return id;
}
std::vector<SelRecord> SelStore::query(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    if (limit >= records_.size()) return records_;
    return {records_.end() - static_cast<std::ptrdiff_t>(limit), records_.end()};
}
std::uint64_t SelStore::next_id() const { std::lock_guard lock(mutex_); return next_id_; }
void SelStore::flush() { std::lock_guard lock(mutex_); }
}
