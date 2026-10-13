#pragma once
#include "core/types.hpp"
#include <functional>
#include <string>

namespace gao {

// What hw reports for one batch of stress work (~250 ms of GPU time).
struct StressBatch {
    long long iterations = 0;   // matmuls completed in this batch
    int wrong_values = 0;       // output elements that differed from the reference
    bool device_lost = false;   // DXGI_ERROR_DEVICE_REMOVED: Windows reset the driver (TDR)
    double elapsed_ms = 0;
};

// Stalled: the run found nothing wrong but computed almost nothing. Returned
// by run_stability when a floor is given and the last kStallWindowS fell below
// it, and by the search when a finished probe scored far below the baseline.
enum class Verdict { Stable, WrongResult, DeviceLost, TooHot, NoTelemetry, Aborted, Stalled };

struct StabilityResult {
    Verdict verdict = Verdict::Stable;
    double score = 0;          // iterations per second over the covered time
    double seconds = 0;        // run time actually covered (sum of batch times)
    int peak_temp_c = -1;
    int end_temp_c = -1;       // the temperature at the last sample; -1 when none was taken
    int end_fan_pct = -1;      // the fan speed at the last sample: what the peak temperature was reached with; -1 when unknown
    int avg_power_w = -1;      // -1 when no sample reported the value
    int avg_core_mhz = -1;
    int avg_mem_mhz = -1;
};

// Why three seconds: a batch delayed by up to about two seconds (a UAC prompt,
// a display mode switch) does not look like a dead card, because the window
// still holds enough normal batches next to it, and a dead card is still
// caught within seconds. A single batch that takes three seconds or more fills
// the window alone and is judged on its own: if it computed little, the run
// ends STALLED.
inline constexpr double kStallWindowS = 3.0;

// The trace of a long measurement, for the log: what the card did while it
// ran, a line at the first reading and one every kTraceEveryS seconds after
// it. The summary of a run gives one clock and one temperature; whether the
// clock fell as the card warmed up is in between.
inline constexpr double kTraceEveryS = 10;
// "    0:10  1716 MHz  78 C  285 W  fan 61 %": the time since the measurement
// began, then the core clock, the temperature, the power and the fan speed.
std::string trace_line(double t_s, const Telemetry& tel);
// True when a line is due for a reading taken t_s seconds into the
// measurement. A gap in the readings gives one line, not one per interval missed.
class TraceClock {
public:
    bool due(double t_s);

private:
    double next_ = 0;
};

// The log line about the card's own temperature target, the point from which
// it slows itself down (GpuControl). target_c, default_c: 0 or less when not
// known. One that is not the default was set by another tool and stays on the
// card when that tool is closed: the card is not at stock then, and the line
// says so. No full stop at the end.
std::string temp_target_text(int target_c, int default_c);

// Runs batches until `seconds` of batch time are covered or something fails,
// whichever comes first. Always runs at least one batch. Stops at the first
// of: lost device, wrong value, missing telemetry, temp_c > max_temp_c, or
// should_stop() returning true. should_stop is asked once per batch (~250 ms)
// after that batch was judged, so a real failure is never reported as
// Aborted. Aborted is neither a pass nor a failure of the settings.
// stall_below: iterations per second; 0 = no floor. When the score over the
// most recent kStallWindowS of batch time is below it, the run ends STALLED.
// Time is the sum of batch elapsed_ms, not the wall clock, so tests are exact.
StabilityResult run_stability(const std::function<StressBatch()>& batch,
                              const std::function<Telemetry()>& read,
                              double seconds, int max_temp_c,
                              const std::function<bool()>& should_stop = {},
                              double stall_below = 0);

const char* verdict_name(Verdict v);

}
