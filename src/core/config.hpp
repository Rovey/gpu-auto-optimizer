#pragma once
#include "core/objectives.hpp"
#include <string>
#include <vector>

namespace gao {

// Everything that has to survive a reboot. One flat struct: there is no second
// version of this format yet, so there is nothing to migrate.
struct Config {
    Preset preset = Preset::BestOfMyGpu;
    Objectives objectives = objectives_for(Preset::BestOfMyGpu);
    int core_offset_mhz = 0;
    int mem_offset_mhz = 0;
    int power_limit_pct = 100;
    std::vector<int> blacklisted_core_offsets;
};

std::string to_json(const Config& c);
Config from_json(const std::string& text);  // never throws; bad input yields defaults

}
