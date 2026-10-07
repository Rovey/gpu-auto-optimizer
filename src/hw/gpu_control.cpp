#include "hw/gpu_control.hpp"
#include "core/vf_curve.hpp"

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
    c.read_vf_curve = [&nvapi, gpu] { return nvapi.ReadVfCurve(gpu); };
    c.write_vf_offsets = [&nvapi, gpu](const std::vector<VfOffset>& offsets) { return nvapi.WriteVfRawOffsets(gpu, offsets); };
    // Stock = offsets 0, the built-in voltage/frequency curve and, where the
    // card allows it, the default power limit. A curve that cannot be read
    // (no administrator rights, or a card without one) is not one this
    // program wrote, and does not fail the reset.
    c.reset_to_stock = [&nvapi, &nvml, gpu, power, read = c.read_vf_curve, write = c.write_vf_offsets] {
        bool curve = true;
        if (read()) {
            GpuControl only_curve;
            only_curve.read_vf_curve = read;
            only_curve.write_vf_offsets = write;
            std::string ignored;
            curve = clear_vf_curve(only_curve, &ignored);
        }
        const bool offsets = nvapi.ResetOffsets(gpu);
        return (power ? nvml.SetPowerLimitPct(gpu, 100) : true) && offsets && curve;
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
    // The offset ranges the driver reports, verified against another tool in
    // hardware check 45. core/search still runs them through plausible_range.
    c.clock_offset_range_mhz = [&nvapi, gpu] { return nvapi.ReadOffsetRangesMhz(gpu); };
    // Fans: only when NVML reports some and a manual range. Empty otherwise,
    // so core code can tell the card has no fan control.
    if (nvml.FanCount(gpu) > 0) {
        if (const auto range = nvml.FanRangePct(gpu)) {
            c.fan_min_pct = range->first;
            c.set_fan_pct = [&nvml, gpu](int pct) { return nvml.SetFanPct(gpu, pct); };
            c.set_fan_auto = [&nvml, gpu] { return nvml.SetFanAuto(gpu); };
            c.read_fan = [&nvml, gpu] { return nvml.ReadFan(gpu); };
        }
    }
    return c;
}

}
