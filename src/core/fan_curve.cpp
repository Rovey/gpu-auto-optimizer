#include "core/fan_curve.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gao {

FanCurve fan_preset_curve(const FanPreset preset) {
    switch (preset) {
        case FanPreset::Silent:     return {50, {{50, 30}, {65, 40}, {75, 55}, {80, 75}}};
        case FanPreset::Cool:       return {45, {{45, 40}, {55, 55}, {65, 80}}};
        case FanPreset::Aggressive: return {std::nullopt, {{40, 40}, {60, 60}, {75, 85}, {83, 100}}};
        case FanPreset::Normal:
        default:                    return {50, {{50, 35}, {60, 45}, {70, 65}, {75, 100}}};
    }
}

const char* fan_preset_name(const FanPreset preset) {
    switch (preset) {
        case FanPreset::Silent:     return "silent";
        case FanPreset::Cool:       return "cool";
        case FanPreset::Aggressive: return "aggressive";
        case FanPreset::Normal:
        default:                    return "normal";
    }
}

std::optional<FanPreset> fan_preset_from_name(const std::string& name) {
    for (FanPreset p : {FanPreset::Silent, FanPreset::Normal, FanPreset::Cool, FanPreset::Aggressive})
        if (name == fan_preset_name(p)) return p;
    return std::nullopt;
}

FanCurve default_curve(const Preset preset) {
    switch (preset) {
        case Preset::Quiet:            return fan_preset_curve(FanPreset::Silent);
        case Preset::CoolAndEfficient: return fan_preset_curve(FanPreset::Cool);
        case Preset::MaxPerformance:   return fan_preset_curve(FanPreset::Aggressive);
        case Preset::BestOfMyGpu:
        default:                       return fan_preset_curve(FanPreset::Normal);
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

FanCommand FanController::decide(const int temp_c, const int power_w, const std::chrono::steady_clock::time_point now) {
    auto driver = [&] {
        last_pct_.reset();
        lower_since_.reset();
        return FanCommand{};
    };
    if (temp_c < 0) return driver();   // no reading: never hold a manual speed blind
    if (temp_c >= max_temp_c_) {
        in_stop_ = false;
        calm_since_.reset();
        last_pct_ = 100;
        lower_since_.reset();
        return FanCommand{false, 100};
    }
    if (curve_.stop_below_c) {
        const int start = *curve_.stop_below_c;
        if (in_stop_) {
            if (temp_c < start) return driver();
            in_stop_ = false;   // warm again: the curve takes over
            if (stopped_at_) {
                const auto off = now - *stopped_at_;
                if (off < kFanPendulum) hold_ = std::min<std::chrono::steady_clock::duration>(hold_ * 2, kFanStopHoldMax);
                else if (off >= kFanCalm) hold_ = kFanStopHold;
            }
        } else if (!last_pct_ && temp_c < start) {
            in_stop_ = true;   // first reading already below: the driver keeps the fans it has
            return driver();
        } else {
            const bool calm = temp_c <= start - kFanStopGapC && power_w >= 0 && power_w < kFanIdlePowerW;
            if (!calm) {
                calm_since_.reset();
            } else if (!calm_since_) {
                calm_since_ = now;
            } else if (now - *calm_since_ >= hold_) {
                in_stop_ = true;
                stopped_at_ = now;
                calm_since_.reset();
                return driver();
            }
        }
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

FanDriver::FanDriver(const GpuControl& gpu, FanCurve curve, const int max_temp_c)
    : gpu_(gpu), curve_(curve), max_temp_c_(max_temp_c), ctrl_(std::move(curve), gpu.fan_min_pct, max_temp_c) {}

void FanDriver::set_curve(FanCurve curve, const int max_temp_c) {
    if (curve == curve_ && max_temp_c == max_temp_c_) return;
    curve_ = curve;
    max_temp_c_ = max_temp_c;
    ctrl_ = FanController(std::move(curve), gpu_.fan_min_pct, max_temp_c);
}

void FanDriver::fail() {
    if (gpu_.set_fan_auto) gpu_.set_fan_auto();   // best effort: the driver is the safe owner
    written_.reset();
    state_ = {FanMode::Failed, 0};
}

void FanDriver::release() {
    if (written_ && gpu_.set_fan_auto) gpu_.set_fan_auto();
    written_.reset();
    if (state_.mode == FanMode::Curve) state_ = {FanMode::Driver, 0};
}

FanState FanDriver::tick(const int temp_c, const int power_w, const std::chrono::steady_clock::time_point now) {
    if (state_.mode == FanMode::Failed || state_.mode == FanMode::Foreign) return state_;
    if (!gpu_.set_fan_pct || !gpu_.set_fan_auto || !gpu_.read_fan) return state_;
    // What does the driver have? Policy back to automatic without us: a driver
    // reset, so write again. Manual at a value we did not write: another tool.
    if (written_) {
        const auto seen = gpu_.read_fan();
        if (!seen) { fail(); return state_; }
        if (!seen->manual) written_.reset();
        else if (seen->target_pct != *written_) { written_.reset(); state_ = {FanMode::Foreign, 0}; return state_; }
    } else if (const auto seen = gpu_.read_fan(); seen && seen->manual) {
        // Manual while we have written nothing (the driver had the fans):
        // another program set them.
        state_ = {FanMode::Foreign, 0};
        return state_;
    }
    const FanCommand cmd = ctrl_.decide(temp_c, power_w, now);
    if (cmd.driver) {
        if (written_ && !gpu_.set_fan_auto()) { fail(); return state_; }
        written_.reset();
        state_ = {FanMode::Driver, 0};
        return state_;
    }
    const bool write = !written_ || std::abs(cmd.pct - *written_) >= 2 || (cmd.pct == 100 && *written_ != 100);
    if (write) {
        if (!gpu_.set_fan_pct(cmd.pct)) { fail(); return state_; }
        written_ = cmd.pct;
    }
    state_ = {FanMode::Curve, *written_};
    return state_;
}

}
