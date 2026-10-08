#pragma once
#include "core/fan_curve.hpp"
#include "core/search.hpp"
#include "core/stability.hpp"
#include "core/types.hpp"
#include <functional>
#include <string>
#include <vector>

namespace gao {

// The quietest fan speed that keeps the card at a target temperature under
// the load it was tuned for, found by measuring, and a fan curve through it.
//
// With the load running, the fans are set to a speed by hand and the card is
// given time to settle; then five percent less, and again, until the card
// settles above the target or the fans' minimum is reached. The answer is the
// lowest speed that held. When the speed it starts from does not hold, it
// goes up instead, to the first that does. A speed is judged when the
// temperature has stopped moving (within 1 C over four looks, 15 s apart),
// or sooner once it is clearly above the target; never after more than three
// minutes.
//
// The target lies kFanTuneMarginC under the profile's temperature limit, so
// that the curve has room to act before the limit is reached. A load that
// stops computing right while the fans are tuned ends the tune: the tune it
// runs on does not hold at this temperature, and that is the caller's to act
// on. Every way out that is not a success hands the fans back to the driver.
inline constexpr int kFanTuneStepPct = 5;
inline constexpr double kFanTuneSliceS = 15;
inline constexpr int kFanTuneSettleSlices = 4;
inline constexpr int kFanTuneMaxSlices = 12;
inline constexpr int kFanTuneMarginC = 5;

// What tune_fan announces while it waits at a speed starts with this; the
// window knows the fan tune by it.
inline constexpr char kFanTuneAnnounce[] = "Fan speed ";
inline bool is_fan_tune_measurement(const std::string& what) { return what.rfind(kFanTuneAnnounce, 0) == 0; }

struct FanTuneIo {
    Probe probe;                                    // one stress run, as in the searches
    std::function<bool()> aborted;
    std::function<void(const std::string&)> log;
    std::function<void(const std::string& what, double seconds)> measuring;   // as in UndervoltIo; optional
};

struct FanTuneStep {
    int pct = 0;
    int temp_c = 0;      // what the card read when the speed was judged
    bool held = false;   // at or under the target
};

struct FanTuneResult {
    bool ok = false;
    std::string reason;      // when !ok; the fans are with the driver then
    int target_c = 0;
    int hold_pct = 0;        // the lowest speed that held the target; the fans are left on it
    int hold_temp_c = 0;
    FanCurve curve;          // through that point; valid when ok
    std::vector<FanTuneStep> steps;
};

// limit_temp_c: the profile's temperature limit. start_pct: where to begin,
// usually the speed the last long run ended on. min_pct: the lowest speed the
// fans hold (learned, or the card's own).
FanTuneResult tune_fan(const GpuControl& gpu, const FanTuneIo& io, int limit_temp_c, int start_pct, int min_pct);

// The curve through the measured point: hold_pct at the target, full speed at
// the limit, and a slow start from where the fans come out of their stop zone.
FanCurve tuned_curve(int target_c, int limit_c, int hold_pct, int min_pct);

}
