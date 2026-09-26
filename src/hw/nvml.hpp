#pragma once
#include "core/types.hpp"
#include <optional>
#include <string>
#include <utility>

namespace gao {

class Nvml {
public:
    bool Init();
    ~Nvml();
    // -1 when NVML isn't initialized or the count could not be read (see
    // Error() for why); a genuine "no GPUs" result is 0. Never conflate the
    // two, for the same reason Telemetry::fan_pct never conflates "unknown"
    // with a real zero reading.
    int DeviceCount();
    Telemetry Read(unsigned index);
    // e.g. "610.74"; empty when NVML cannot report it (see Error()).
    std::string DriverVersion();
    // The card's NVML UUID ("GPU-..."); empty when unavailable.
    std::string GpuUuid(unsigned index);
    // Power limit as percent of the driver default. Empty when NVML cannot
    // report the constraints (older cards, or a failed call; see Error()).
    std::optional<std::pair<int, int>> PowerLimitRangePct(unsigned index);
    // Sets the limit to pct of default and verifies by reading it back
    // (within 1 % of default). Needs administrator rights.
    bool SetPowerLimitPct(unsigned index, int pct);
    // Current limit as a (rounded) percent of the default; nullopt on failure.
    std::optional<int> PowerLimitPct(unsigned index);
    const std::string& Error() const { return error_; }

private:
    void* lib_ = nullptr;
    bool inited_ = false;
    std::string error_;
};

}
