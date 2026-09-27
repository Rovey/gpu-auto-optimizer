#include "core/objectives.hpp"

namespace gao {

Objectives objectives_for(const Preset preset) {
    switch (preset) {
        case Preset::Quiet:            return {80, 0.4f, true,  true,  true};
        case Preset::CoolAndEfficient: return {65, 0.3f, false, false, true};
        case Preset::MaxPerformance:   return {83, 1.0f, true,  true,  true};
        case Preset::BestOfMyGpu:
        default:                       return {75, 0.7f, true,  true,  true};
    }
}

const char* preset_name(const Preset preset) {
    switch (preset) {
        case Preset::Quiet:            return "quiet";
        case Preset::CoolAndEfficient: return "cool";
        case Preset::MaxPerformance:   return "max";
        case Preset::BestOfMyGpu:
        default:                       return "best";
    }
}

std::optional<Preset> preset_from_name(const std::string& name) {
    for (Preset p : {Preset::BestOfMyGpu, Preset::Quiet, Preset::CoolAndEfficient, Preset::MaxPerformance})
        if (name == preset_name(p)) return p;
    return std::nullopt;
}

}
