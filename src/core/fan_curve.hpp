#pragma once
// Fan control as data and decisions: the curve, the per-second controller and
// the driver that writes through GpuControl. No hardware here; NVML lives in
// hw/nvml and reaches this code only through GpuControl.
#include "core/objectives.hpp"
#include "core/types.hpp"
#include <chrono>
#include <optional>
#include <string>
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

// Leaving the curve for the fan-stop zone. A card under light load cools a
// few degrees with the fans on and warms past the threshold with them off, so
// a temperature gap alone makes the fans cycle every few seconds. They stop
// only when the card is well below the threshold AND idle for a hold time, and
// the hold doubles each time the fans had to restart soon after stopping.
inline constexpr int kFanStopGapC = 8;                                  // stop only 8 C below the threshold
inline constexpr int kFanIdlePowerW = 30;                               // and below this power draw
inline constexpr auto kFanStopHold = std::chrono::seconds(60);          // for this long, at first
inline constexpr auto kFanStopHoldMax = std::chrono::minutes(15);
inline constexpr auto kFanPendulum = std::chrono::minutes(5);           // a restart this soon doubles the hold
inline constexpr auto kFanCalm = std::chrono::minutes(10);              // stopped this long: back to the first hold
// Fans may not hold the minimum NVML reports: the reference RTX 4070 reports
// 30 % but stalls and restarts below 50 %. A fan that measures 0 % after this
// long at a manual speed has stalled, and the minimum goes up a step.
inline constexpr auto kFanStallGrace = std::chrono::seconds(4);
inline constexpr int kFanStallStep = 5;
inline constexpr int kFanStallMax = 70;   // still stalling above this: something else is wrong
inline constexpr auto kFanSlowDown = std::chrono::seconds(5);   // a lower speed must hold this long

// Fan curves by character, independent of the tuning profile: Best of my GPU
// can run with Silent, Cool & efficient with Normal.
enum class FanPreset { Silent, Normal, Cool, Aggressive };
FanCurve fan_preset_curve(FanPreset preset);
const char* fan_preset_name(FanPreset preset);   // "silent", "normal", "cool", "aggressive"
std::optional<FanPreset> fan_preset_from_name(const std::string& name);
// The profile's own fan preset: Quiet -> Silent, Best -> Normal, Cool -> Cool, Max -> Aggressive.
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
    // power_w: -1 when unknown, which counts as load.
    FanCommand decide(int temp_c, int power_w, std::chrono::steady_clock::time_point now);
    void set_min_pct(int min_pct) { min_pct_ = min_pct; }

private:
    FanCurve curve_;
    int min_pct_;
    int max_temp_c_;
    bool in_stop_ = false;
    std::optional<int> last_pct_;
    std::optional<std::chrono::steady_clock::time_point> lower_since_;
    std::optional<std::chrono::steady_clock::time_point> calm_since_;   // cool and idle since
    std::optional<std::chrono::steady_clock::time_point> stopped_at_;   // when we last let the fans stop
    std::chrono::steady_clock::duration hold_ = kFanStopHold;
};

enum class FanMode {
    Driver,    // the driver controls the fans (fan-stop zone, no reading, or not started)
    Curve,     // we set a manual speed
    Foreign,   // another program set a manual speed; we stepped aside
    Failed,    // a write did not verify; handed back and stopped
};

struct FanState {
    FanMode mode = FanMode::Driver;
    int pct = 0;       // the manual speed, when mode is Curve
    int min_pct = 0;   // the lowest manual speed in use (NVML's, or higher once learned)
};

// Writes a FanController's decisions through GpuControl, once a second.
// Writes only on a change of 2 % or more (or to reach 100 %), re-applies after
// a driver reset, steps aside when another program set the fans, and hands the
// fans back to the driver on release() or on any failed write.
class FanDriver {
public:
    // min_pct: a minimum learned earlier for this card; never below NVML's own.
    FanDriver(const GpuControl& gpu, FanCurve curve, int max_temp_c, int min_pct = 0);
    FanState tick(int temp_c, int power_w, std::chrono::steady_clock::time_point now);
    // Keeps what was written, so no false "another program"; an unchanged
    // curve keeps the controller's hysteresis and slow-down state too.
    void set_curve(FanCurve curve, int max_temp_c);
    void release();
    FanState state() const { return state_; }

private:
    void fail();
    const GpuControl& gpu_;
    FanCurve curve_;
    int max_temp_c_;
    FanController ctrl_;
    FanState state_;
    std::optional<int> written_;   // the manual speed we set, while it is ours
    std::optional<std::chrono::steady_clock::time_point> written_at_;
    int min_pct_;
};

}
