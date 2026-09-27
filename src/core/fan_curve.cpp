#include "core/fan_curve.hpp"
#include <algorithm>
#include <cmath>

namespace gao {

FanCurve default_curve(const Preset preset) {
    switch (preset) {
        case Preset::Quiet:            return {50, {{50, 30}, {65, 40}, {75, 55}, {80, 75}}};
        case Preset::CoolAndEfficient: return {45, {{45, 40}, {55, 55}, {65, 80}}};
        case Preset::MaxPerformance:   return {std::nullopt, {{40, 40}, {60, 60}, {75, 85}, {83, 100}}};
        case Preset::BestOfMyGpu:
        default:                       return {50, {{50, 35}, {60, 45}, {70, 65}, {75, 100}}};
    }
}

bool valid(const FanCurve& c) {
    if (c.points.size() < 2 || c.points.size() > 6) return false;
    if (c.stop_below_c && (*c.stop_below_c < 20 || *c.stop_below_c > 90)) return false;
    for (size_t i = 0; i < c.points.size(); ++i) {
        const FanPoint& p = c.points[i];
        if (p.temp_c < 0 || p.temp_c > 110 || p.pct < 0 || p.pct > 100) return false;
        if (i > 0 && (p.temp_c <= c.points[i - 1].temp_c || p.pct < c.points[i - 1].pct)) return false;
    }
    return true;
}

int curve_pct(const FanCurve& c, const int temp_c) {
    const auto& pts = c.points;
    if (temp_c <= pts.front().temp_c) return pts.front().pct;
    if (temp_c >= pts.back().temp_c) return pts.back().pct;
    const auto hi = std::find_if(pts.begin(), pts.end(), [&](const FanPoint& p) { return p.temp_c >= temp_c; });
    const auto lo = hi - 1;
    const double f = static_cast<double>(temp_c - lo->temp_c) / (hi->temp_c - lo->temp_c);
    return static_cast<int>(std::lround(lo->pct + f * (hi->pct - lo->pct)));
}

bool quieter_than(const FanCurve& a, const FanCurve& b, const int max_temp_c) {
    auto effective = [](const FanCurve& c, int t) { return c.stop_below_c && t < *c.stop_below_c ? 0 : curve_pct(c, t); };
    for (int t = 30; t <= max_temp_c; ++t)
        if (effective(a, t) < effective(b, t)) return true;
    return false;
}

FanController::FanController(FanCurve curve, const int min_pct, const int max_temp_c)
    : curve_(std::move(curve)), min_pct_(min_pct), max_temp_c_(max_temp_c) {}

FanCommand FanController::decide(const int temp_c, const std::chrono::steady_clock::time_point now) {
    auto driver = [&] {
        last_pct_.reset();
        lower_since_.reset();
        return FanCommand{};
    };
    if (temp_c < 0) return driver();   // no reading: never hold a manual speed blind
    if (temp_c >= max_temp_c_) {
        in_stop_ = false;
        last_pct_ = 100;
        lower_since_.reset();
        return FanCommand{false, 100};
    }
    if (curve_.stop_below_c) {
        const int stop = *curve_.stop_below_c;
        if (in_stop_ && temp_c >= stop) in_stop_ = false;
        else if (!in_stop_ && temp_c <= stop - kFanHysteresisC) in_stop_ = true;
        else if (!in_stop_ && !last_pct_ && temp_c < stop) in_stop_ = true;   // first reading already below
        if (in_stop_) return driver();
    }
    const int target = std::max(min_pct_, curve_pct(curve_, temp_c));
    if (!last_pct_ || target >= *last_pct_) {
        last_pct_ = target;
        lower_since_.reset();
    } else if (!lower_since_) {
        lower_since_ = now;
    } else if (now - *lower_since_ >= kFanSlowDown) {
        last_pct_ = target;
        lower_since_.reset();
    }
    return FanCommand{false, *last_pct_};
}

}
