#pragma once
#include "core/types.hpp"
#include <functional>

namespace gao {

// What hw reports for one batch of stress work (~250 ms of GPU time).
struct StressBatch {
    long long iterations = 0;   // matmuls completed in this batch
    int wrong_values = 0;       // output elements that differed from the reference
    bool device_lost = false;   // DXGI_ERROR_DEVICE_REMOVED: Windows reset the driver (TDR)
    double elapsed_ms = 0;
};

enum class Verdict { Stable, WrongResult, DeviceLost, TooHot, NoTelemetry };

struct StabilityResult {
    Verdict verdict = Verdict::Stable;
    double score = 0;          // iterations per second over the covered time
    double seconds = 0;        // run time actually covered (sum of batch times)
    int peak_temp_c = -1;
    int avg_power_w = -1;      // -1 when no sample reported the value
    int avg_core_mhz = -1;
    int avg_mem_mhz = -1;
};

// Runs batches until `seconds` of batch time are covered or something fails,
// whichever comes first. Always runs at least one batch. Stops at the first
// of: lost device, wrong value, missing telemetry, temp_c > max_temp_c.
// Time is the sum of batch elapsed_ms, not the wall clock, so tests are exact.
StabilityResult run_stability(const std::function<StressBatch()>& batch,
                              const std::function<Telemetry()>& read,
                              double seconds, int max_temp_c);

const char* verdict_name(Verdict v);

}
