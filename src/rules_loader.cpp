#include "bmc/core.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bmc {
std::vector<FaultRule> load_rules(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open rules: " + path.string());
    std::ostringstream content;
    content << input.rdbuf();
    return parse_rules(content.str());
}
}
