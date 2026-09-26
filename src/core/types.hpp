#pragma once
#include <functional>
#include <utility>
#include <vector>

namespace gao {

struct Telemetry {
    int core_mhz = -1;        // -1 when the driver does not report it
    int mem_mhz = -1;         // -1 when the driver does not report it
    int temp_c = -1;          // -1 when the driver does not report it
    int fan_pct = -1;         // -1 when the driver does not report it
    int power_w = -1;         // -1 when the driver does not report it
    int power_limit_w = 0;
    // True only when the two readings the search depends on -- core clock
    // and temperature -- both came back real. A device handle alone is not
    // enough: without this, a missing/failing sensor call reads back as a
    // plausible zero (a "cold card") instead of the unknown it actually is,
    // and the spec's 85 C thermal abort would never fire against a stuck
    // temp_c of 0. The other fields may still be -1 (unknown) even when ok
    // is true; check each field individually, the same way fan_pct works.
    bool ok = false;
};

struct FanPoint { int temp_c; int fan_pct; };
using FanCurve = std::vector<FanPoint>;   // four points, ascending by temp_c

// The only way core code reaches hardware. hw/ fills these in production,
// tests fill them with lambdas. An empty callback means "not supported here".
struct GpuControl {
    std::function<Telemetry()> read;
    std::function<bool(int)> set_core_offset;   // MHz, verified by read-back
    std::function<bool(int)> set_mem_offset;    // MHz, verified by read-back
    std::function<bool(int)> set_power_limit;   // percent of default
    std::function<std::pair<int, int>()> power_limit_range_pct;   // {min, max}, percent of default
    std::function<bool(int)> set_fan_pct;       // percent, -1 restores automatic
    std::function<bool()> reset_to_stock;
};

}
