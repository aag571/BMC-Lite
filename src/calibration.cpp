#include "bmc/core.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bmc {
double apply(const Calibration& calibration, double raw) {
    if (!std::isfinite(raw) || !std::isfinite(calibration.gain) || !std::isfinite(calibration.offset)) {
        throw std::invalid_argument("non-finite calibration input");
    }
    const double corrected = raw * calibration.gain + calibration.offset;
    if (calibration.points.empty()) {
        return corrected;
    }
    // 校准点按 raw 升序存放，validate() 已保证严格递增。
    if (corrected <= calibration.points.front().first) {
        return calibration.points.front().second;
    }
    if (corrected >= calibration.points.back().first) {
        return calibration.points.back().second;
    }
    const auto upper = std::upper_bound(calibration.points.begin(), calibration.points.end(), corrected,
        [](double value, const std::pair<double, double>& point) { return value < point.first; });
    const auto lower = upper - 1;
    const double span = upper->first - lower->first;
    const double ratio = (corrected - lower->first) / span;
    return lower->second + ratio * (upper->second - lower->second);
}
}
