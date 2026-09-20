#pragma once
#include <functional>
#include <vector>

namespace gao {

struct Telemetry {
    int core_mhz = 0;
    int mem_mhz = 0;
    int temp_c = 0;
    int fan_pct = -1;         // -1 when the driver does not report it
    int power_w = 0;
    int power_limit_w = 0;
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
    std::function<bool(int)> set_fan_pct;       // percent, -1 restores automatic
    std::function<bool()> reset_to_stock;
};

}
