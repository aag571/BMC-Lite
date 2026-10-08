#include "bmc/core.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bmc {
void validate(const Config& config) {
    if (config.id.empty() || config.path.empty()) {
        throw std::invalid_argument("sensor id/path cannot be empty");
    }
    if (config.backend != "mock" && config.backend != "sysfs" && config.backend != "i2c" && config.backend != "gpio") {
        throw std::invalid_argument("unknown backend: " + config.backend);
    }
    if (!std::isfinite(config.scale) || config.scale == 0 || !std::isfinite(config.warning) || !std::isfinite(config.critical) || !std::isfinite(config.hysteresis)) {
        throw std::invalid_argument("non-finite sensor policy");
    }
    const bool ordered = config.high ? config.warning < config.critical : config.warning > config.critical;
    if (!ordered || config.hysteresis < 0 || config.hysteresis >= std::abs(config.critical - config.warning) || config.debounce == 0 || config.failure_limit == 0) {
        throw std::invalid_argument("invalid sensor thresholds/windows");
    }
}
std::vector<Config> load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open config: " + path.string());
    }
    std::vector<Config> result;
    std::set<std::string> ids;
    std::string line;
    unsigned line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos || line[line.find_first_not_of(" \t\r")] == '#') {
            continue;
        }
        Config config;
        std::string direction;
        std::string trailing;
        std::string debounce;
        std::string failures;
        std::istringstream fields(line);
        if (!(fields >> config.id >> config.backend >> config.path >> config.scale >> direction >> config.warning >> config.critical >> config.hysteresis >> debounce >> failures >> config.action_path) || (fields >> trailing)) {
            throw std::invalid_argument("invalid config line " + std::to_string(line_number));
        }
        const auto parse_window = [](const std::string& token) {
            if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char character) { return character >= '0' && character <= '9'; })) {
                throw std::invalid_argument("invalid window");
            }
            const auto value = std::stoul(token);
            if (value == 0 || value > 1000000) {
                throw std::invalid_argument("window outside supported range");
            }
            return static_cast<unsigned>(value);
        };
        config.debounce = parse_window(debounce);
        config.failure_limit = parse_window(failures);
        if (direction != "high" && direction != "low") {
            throw std::invalid_argument("invalid threshold direction");
        }
        config.high = direction == "high";
        if (config.action_path == "-") {
            config.action_path.clear();
        }
        validate(config);
        if (!ids.insert(config.id).second) {
            throw std::invalid_argument("duplicate sensor: " + config.id);
        }
        result.push_back(config);
    }
    if (result.empty()) {
        throw std::invalid_argument("no sensors configured");
    }
    return result;
}
}
