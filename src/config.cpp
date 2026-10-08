#include "bmc/chip.hpp"
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
        // i2c:<chip> 形态按前缀放行，具体型号在设备构造时校验。
        if (config.backend.rfind("i2c:", 0) != 0) {
            throw std::invalid_argument("unknown backend: " + config.backend);
        }
    }
    if (!std::isfinite(config.scale) || config.scale == 0 || !std::isfinite(config.warning) || !std::isfinite(config.critical) || !std::isfinite(config.hysteresis)) {
        throw std::invalid_argument("non-finite sensor policy");
    }
    const bool ordered = config.high ? config.warning < config.critical : config.warning > config.critical;
    if (!ordered || config.hysteresis < 0 || config.hysteresis >= std::abs(config.critical - config.warning) || config.debounce == 0 || config.failure_limit == 0) {
        throw std::invalid_argument("invalid sensor thresholds/windows");
    }
    // 仅在该传感器确实配置了标定时才要求有限；gain=1/offset=0 的默认值本就有限。
    if (!std::isfinite(config.calibration.gain) || !std::isfinite(config.calibration.offset)) {
        throw std::invalid_argument("non-finite calibration gain/offset");
    }
    double previous = 0;
    bool first_point = true;
    for (const auto& point : config.calibration.points) {
        if (!std::isfinite(point.first) || !std::isfinite(point.second)) {
            throw std::invalid_argument("non-finite calibration point");
        }
        if (!first_point && point.first <= previous) {
            throw std::invalid_argument("calibration points must be strictly increasing in raw value");
        }
        previous = point.first;
        first_point = false;
    }
    if (config.backend.rfind("i2c:", 0) == 0) {
        const std::string chip = config.backend.substr(4);
        const auto known = chip_names();
        if (std::find(known.begin(), known.end(), chip) == known.end()) {
            throw std::invalid_argument("unknown i2c chip: " + chip);
        }
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
        std::string calibration;
        std::istringstream fields(line);
        if (!(fields >> config.id >> config.backend >> config.path >> config.scale >> direction >> config.warning >> config.critical >> config.hysteresis >> debounce >> failures >> config.action_path)) {
            throw std::invalid_argument("invalid config line " + std::to_string(line_number));
        }
        // 第 12 个字段（标定）可选，因此旧配置仍然可用。
        if (!(fields >> calibration)) {
            calibration.clear();
        }
        if (fields >> trailing) {
            throw std::invalid_argument("invalid config line " + std::to_string(line_number));
        }
        const auto parse_calibration = [](const std::string& token) {
            Calibration parsed;
            if (token.empty() || token == "-") {
                return parsed;
            }
            // 语法：gain[:offset][;raw=value;raw=value...]
            std::size_t position = token.find(';');
            const std::string head = token.substr(0, position);
            if (!head.empty()) {
                const std::size_t colon = head.find(':');
                parsed.gain = std::stod(head.substr(0, colon));
                if (colon != std::string::npos) {
                    parsed.offset = std::stod(head.substr(colon + 1));
                }
            }
            while (position != std::string::npos) {
                const std::size_t next = token.find(';', position + 1);
                const std::string pair = token.substr(position + 1, next == std::string::npos ? std::string::npos : next - position - 1);
                position = next;
                if (pair.empty()) {
                    continue;
                }
                const std::size_t equals = pair.find('=');
                if (equals == std::string::npos) {
                    throw std::invalid_argument("invalid calibration point");
                }
                parsed.points.emplace_back(std::stod(pair.substr(0, equals)), std::stod(pair.substr(equals + 1)));
            }
            return parsed;
        };
        try {
            config.calibration = parse_calibration(calibration);
        } catch (const std::exception&) {
            throw std::invalid_argument("invalid calibration on line " + std::to_string(line_number));
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
