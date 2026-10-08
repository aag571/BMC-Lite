#include "bmc/chip.hpp"
#include "bmc/core.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

// 传感器配置文件（hardware.conf 形态）的解析与校验，声明见 bmc/core.hpp。
// 本文件只负责把文本变成 Config 列表：字段顺序、可选标定字段与全部策略约束都在这里闭环；
// 它不触碰任何设备，具体后端的读取交给 readers.cpp / chips.cpp。
namespace bmc {
// 校验单个传感器的策略：空 id/path、未知后端、非有限阈值、迟滞与窗口都在这层拒绝。
// 失败即抛 std::invalid_argument，load_config 会把它翻成带行号的配置错误。
void validate(const Config& config) {
    if (config.id.empty() || config.path.empty()) {
        throw std::invalid_argument("sensor id/path cannot be empty");
    }
    if (config.backend != "mock" && config.backend != "sysfs" && config.backend != "i2c" && config.backend != "gpio") {
        // i2c:<chip>[@<feature>] 形态在这里只做前缀放行；型号名由本函数的末尾与
        // chip_names() 比对，测量量名由 readers.cpp 构造驱动时校验（见 make_chip）。
        if (config.backend.rfind("i2c:", 0) != 0) {
            throw std::invalid_argument("unknown backend: " + config.backend);
        }
    }
    // NaN 参与比较恒为 false，会让阈值判定与去抖静默失效；scale==0 则把所有读数压成 0。
    if (!std::isfinite(config.scale) || config.scale == 0 || !std::isfinite(config.warning) || !std::isfinite(config.critical) || !std::isfinite(config.hysteresis)) {
        throw std::invalid_argument("non-finite sensor policy");
    }
    // high/low 决定两档阈值的先后关系，配置必须与之自洽。
    const bool ordered = config.high ? config.warning < config.critical : config.warning > config.critical;
    // 迟滞必须严格小于两档间距：否则较严重档的恢复阈值（临界值减去迟滞）会越过较轻档的进入
    // 阈值，较轻档永远不可达。debounce/failure_limit 为 0 则状态机无法累积任何确认。
    if (!ordered || config.hysteresis < 0 || config.hysteresis >= std::abs(config.critical - config.warning) || config.debounce == 0 || config.failure_limit == 0) {
        throw std::invalid_argument("invalid sensor thresholds/windows");
    }
    // 仅在该传感器确实配置了标定时才要求有限；gain=1/offset=0 的默认值本就有限。
    if (!std::isfinite(config.calibration.gain) || !std::isfinite(config.calibration.offset)) {
        throw std::invalid_argument("non-finite calibration gain/offset");
    }
    // gain 必须为正：标定点按 raw 严格递增存放，而 apply() 查表用的是
    // corrected = raw * gain + offset。gain < 0 会让同一张表在查表域里变成递减，
    // 区间查找随即落进错误的段并静默返回错误值；gain == 0 则把所有读数压成同一点。
    if (config.calibration.gain <= 0) {
        throw std::invalid_argument("calibration gain must be positive");
    }
    // 标定点的 raw 值必须严格递增：apply() 用 upper_bound 做分段线性插值，
    // 乱序或重复的点会让区间查找落进错误的段；区间之外的值则按端点钳制。
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
    // 芯片型号在此按驱动注册表（chip_names）校验，让配置里的拼写错误在装载阶段就暴露，
    // 而不是等到设备构造或首次采样。后端语法是 i2c:<chip>[@<feature>]，
    // 因此只取 "@" 之前的型号名；<feature> 属于测量量，由构造期 make_chip 校验
    // （见 readers.cpp 的 ChipReader）。
    if (config.backend.rfind("i2c:", 0) == 0) {
        const std::string chip = config.backend.substr(4, config.backend.find('@') - 4);
        const auto known = chip_names();
        if (std::find(known.begin(), known.end(), chip) == known.end()) {
            throw std::invalid_argument("unknown i2c chip: " + chip);
        }
    }
}
// 逐行读取配置文件并构造 Config。每行的磁盘字段顺序固定为
// id backend path scale direction warning critical hysteresis debounce failure_limit action_path
// 后面可再跟一个可选的标定字段；字段缺失或多出都按整行非法处理。
std::vector<Config> load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open config: " + path.string());
    }
    std::vector<Config> result;
    std::set<std::string> ids;
    std::string line;
    // 先自增行号再决定是否跳过，保证报错里的行号与编辑器显示的行号一致。
    unsigned line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        // 只认行首（可带前导空白）的 # 为注释；行内 # 属于字段内容。
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
        // 前 11 个字段全部必填，任何一处解析失败都视作整行非法。
        if (!(fields >> config.id >> config.backend >> config.path >> config.scale >> direction >> config.warning >> config.critical >> config.hysteresis >> debounce >> failures >> config.action_path)) {
            throw std::invalid_argument("invalid config line " + std::to_string(line_number));
        }
        // 第 12 个字段（标定）可选，因此旧配置仍然可用。
        if (!(fields >> calibration)) {
            calibration.clear();
        }
        // 多出的字段直接报错：静默忽略会掩盖字段错位或多余的空白字段。
        if (fields >> trailing) {
            throw std::invalid_argument("invalid config line " + std::to_string(line_number));
        }
        // 把标定字段解析成 Calibration；"-" 与空串都表示"没有标定"。
        // 修正顺序是先做 gain/offset 线性修正，再按修正后的值查表插值（见 calibration.cpp）。
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
        // 标定子解析器抛出的 std::exception 在这里统一转成带行号的错误，避免丢掉位置信息。
        try {
            config.calibration = parse_calibration(calibration);
        } catch (const std::exception&) {
            throw std::invalid_argument("invalid calibration on line " + std::to_string(line_number));
        }
        // 窗口只接受纯数字：std::stoul 会接受 "3abc" 这类半截数字，这里直接拒绝。
        // 上限 1000000 既保证窗口实际可达，也让确认计数器的上限可控。
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
        // direction 是方向的唯一来源：high 表示越大越危险，low 表示越小越危险。
        if (direction != "high" && direction != "low") {
            throw std::invalid_argument("invalid threshold direction");
        }
        config.high = direction == "high";
        // "-" 是显式的"没有动作路径"，与空字段区分开，避免把路径名误当成占位符。
        if (config.action_path == "-") {
            config.action_path.clear();
        }
        validate(config);
        // 同一 id 出现两次一律拒绝：规则匹配、控制面动作路径与 Prometheus 标签都按传感器 id
        // 索引，重复 id 会让这些映射产生歧义；SEL 里也会出现两条互相矛盾的迁移记录。
        if (!ids.insert(config.id).second) {
            throw std::invalid_argument("duplicate sensor: " + config.id);
        }
        result.push_back(config);
    }
    // 空配置视作错误：没有传感器时守护进程没有任何可做的事，启动即失败好过静默空转。
    if (result.empty()) {
        throw std::invalid_argument("no sensors configured");
    }
    return result;
}
}
