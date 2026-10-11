#pragma once
#include "core/busy_card.hpp"
#include <optional>
#include <string>
#include <vector>

namespace gao {

// What every other process uses of the NVIDIA card, per engine, averaged over
// `samples` looks one second apart: the numbers Task Manager shows per
// process, from the same Windows performance counters. Needs no elevation.
// Empty when Windows does not say (no such counters, or they cannot be read):
// what cannot be known reads as nobody, since this is advice and no check a
// run relies on.
std::vector<GpuUse> gpu_use(int samples = 3);

// One instance of the "GPU Engine" counter, from its name:
// "pid_4100_luid_0x00000000_0x0000E9C1_phys_0_eng_0_engtype_3D".
struct GpuEngineInstance {
    unsigned pid = 0;
    unsigned long long luid = 0;   // of the adapter it runs on
    std::string engine;            // "3D", "VideoDecode", ...
};
std::optional<GpuEngineInstance> parse_gpu_engine_instance(const std::wstring& name);

}
