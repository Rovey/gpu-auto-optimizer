#include "hw/gpu_control.hpp"

namespace gao {

// Wires the two drivers into the single struct core code sees. Callbacks the
// hardware cannot support are left empty on purpose, so callers can tell.
GpuControl make_gpu_control(Nvml& nvml, Nvapi& nvapi, unsigned gpu) {
    GpuControl c;
    c.read = [&nvml, gpu] { return nvml.Read(gpu); };
    c.set_core_offset = [&nvapi, gpu](int mhz) { return nvapi.SetCoreOffsetMhz(gpu, mhz); };
    c.set_mem_offset = [&nvapi, gpu](int mhz) { return nvapi.SetMemOffsetMhz(gpu, mhz); };
    const bool power = nvml.PowerLimitRangePct(gpu).has_value();
    if (power) {
        c.set_power_limit = [&nvml, gpu](int pct) { return nvml.SetPowerLimitPct(gpu, pct); };
        // {0, 0} when the query fails later on; the power step skips such a range.
        c.power_limit_range_pct = [&nvml, gpu] { return nvml.PowerLimitRangePct(gpu).value_or(std::pair{0, 0}); };
    }
    // Stock = offsets 0 and, where the card allows it, the default power limit.
    c.reset_to_stock = [&nvapi, &nvml, gpu, power] {
        const bool offsets = nvapi.ResetOffsets(gpu);
        return (power ? nvml.SetPowerLimitPct(gpu, 100) : true) && offsets;
    };
    // What is applied right now, straight from the driver. A card without
    // power control counts as 100 %.
    c.read_applied = [&nvapi, &nvml, gpu, power]() -> std::optional<AppliedState> {
        const auto offsets = nvapi.ReadOffsetsMhz(gpu);
        if (!offsets) return std::nullopt;
        const auto pct = power ? nvml.PowerLimitPct(gpu) : std::optional<int>(100);
        if (!pct) return std::nullopt;
        return AppliedState{offsets->first, offsets->second, *pct};
    };
    return c;
}

}
