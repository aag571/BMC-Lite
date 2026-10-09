#include "bmc/core.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

// 传感器读数标定：先做 gain/offset 线性修正，再按校准点做分段线性插值。
// 由 include/bmc/core.hpp 声明的 Calibration 与 apply 在这里实现。
namespace bmc {
// 唯一的标定入口；非有限输入在这里就拦下，不让 NaN 传播到阈值比较被静默判成 normal。
double apply(const Calibration& calibration, double raw) {
    // 三个输入任一非有限都拒绝：NaN 参与比较永远为假，会一路隐形地影响判定。
    if (!std::isfinite(raw) || !std::isfinite(calibration.gain) || !std::isfinite(calibration.offset)) {
        throw std::invalid_argument("non-finite calibration input");
    }
    // 顺序不可交换：校准点定义在线性修正之后的量纲上，查表前必须先做这一步换算。
    const double corrected = raw * calibration.gain + calibration.offset;
    if (!std::isfinite(corrected)) {
        throw std::invalid_argument("non-finite calibrated value");
    }
    // 没有校准点即纯线性转换，与只配置 scale 的行为一致。
    if (calibration.points.empty()) {
        return corrected;
    }
    // 校准点按 raw 升序存放，validate() 已保证严格递增。
    // 低于最左点直接取该点：范围外夹紧而不外推，外推会把未标定区间的误差放大。
    if (corrected <= calibration.points.front().first) {
        return calibration.points.front().second;
    }
    // 上界同理；两端分开判断，是为了让 corrected 恰等于端点时返回该点的值。
    if (corrected >= calibration.points.back().first) {
        return calibration.points.back().second;
    }
    // upper_bound 给出第一个键大于 corrected 的点，lower 因此是最后一个不超过它的点。
    // 比较器参数顺序必须与 upper_bound 的约定一致；写反就是 lower_bound 的语义，插值区间取错。
    const auto upper = std::upper_bound(calibration.points.begin(), calibration.points.end(), corrected,
        [](double value, const std::pair<double, double>& point) { return value < point.first; });
    const auto lower = upper - 1;
    const double span = upper->first - lower->first;
    const double ratio = (corrected - lower->first) / span;
    return lower->second + ratio * (upper->second - lower->second);
}
}
