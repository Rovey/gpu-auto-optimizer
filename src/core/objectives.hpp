#pragma once
#include <optional>
#include <string>

namespace gao {

enum class Preset { BestOfMyGpu, Quiet, CoolAndEfficient, MaxPerformance };

// What the user wants, not what the tuner will do. The search reads these as
// ceilings; perf_push decides how hard it chases clocks inside them.
struct Objectives {
    int   max_temp_c = 75;
    int   max_fan_pct = 60;
    float perf_push = 0.7f;   // 0..1
    bool  core_oc = true;
    bool  mem_oc = true;
    bool  power = true;
    bool  undervolt = false;  // opt-in only, never set by a preset
};

Objectives objectives_for(Preset preset);

// Short names used on the command line and in gao.json.
const char* preset_name(Preset preset);
std::optional<Preset> preset_from_name(const std::string& name);

}
