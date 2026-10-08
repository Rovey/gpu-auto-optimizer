#pragma once
#include <optional>
#include <string>

namespace gao {

// Undervolt is not a clock search: the stock speed on less voltage (core/undervolt.hpp).
// AllInOne is three runs in a row: the overclock of BestOfMyGpu, an undervolt at a
// clock between stock and that overclock, and the fan tune (app/common.hpp).
enum class Preset { BestOfMyGpu, Quiet, CoolAndEfficient, MaxPerformance, Undervolt, AllInOne };

// What the user wants, not what the tuner will do. The search reads these as
// ceilings; perf_push decides how hard it chases clocks inside them.
struct Objectives {
    int   max_temp_c = 75;
    float perf_push = 0.7f;   // 0..1
    bool  core_oc = true;
    bool  mem_oc = true;
    bool  power = true;
};

Objectives objectives_for(Preset preset);

// Short names used on the command line and in gao.json.
const char* preset_name(Preset preset);
std::optional<Preset> preset_from_name(const std::string& name);

}
