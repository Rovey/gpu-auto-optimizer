#pragma once
// Fan control as data and decisions: the curve, the per-second controller and
// the driver that writes through GpuControl. No hardware here; NVML lives in
// hw/nvml and reaches this code only through GpuControl.
#include "core/objectives.hpp"
#include <chrono>
#include <optional>
#include <vector>

namespace gao {

struct FanPoint {
    int temp_c;
    int pct;
    bool operator==(const FanPoint&) const = default;
};

struct FanCurve {
    std::optional<int> stop_below_c;   // below this the driver controls the fans (it stops them at idle)
    std::vector<FanPoint> points;      // 2-6, ascending temperature, non-decreasing fan %
    bool operator==(const FanCurve&) const = default;
};

inline constexpr int kFanHysteresisC = 3;                       // leave the curve 3 C below the stop threshold
inline constexpr auto kFanSlowDown = std::chrono::seconds(5);   // a lower speed must hold this long

FanCurve default_curve(Preset preset);
bool valid(const FanCurve& c);
// The curve's own value at temp_c: linear between points, rounded to the
// nearest percent, the first/last point's value beyond the ends. No guard rails.
int curve_pct(const FanCurve& c, int temp_c);
// True when a gives a lower fan % than b at any temperature from 30 C up to
// max_temp_c, counting the fan-stop zone as 0 %.
bool quieter_than(const FanCurve& a, const FanCurve& b, int max_temp_c);

struct FanCommand {
    bool driver = true;   // hand the fans to the driver
    int pct = 0;          // otherwise: this manual speed
};

// Turns a temperature into a fan command once a second. Holds the fan-stop
// hysteresis and the "fast up, slow down" state, and applies the guard rails.
class FanController {
public:
    FanController(FanCurve curve, int min_pct, int max_temp_c);
    FanCommand decide(int temp_c, std::chrono::steady_clock::time_point now);

private:
    FanCurve curve_;
    int min_pct_;
    int max_temp_c_;
    bool in_stop_ = false;
    std::optional<int> last_pct_;
    std::optional<std::chrono::steady_clock::time_point> lower_since_;
};

}
